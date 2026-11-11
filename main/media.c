#include "media.h"
#include "board.h"
#include "usb_disk.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_extractor.h"
#include "esp_mp4_extractor.h"
#include "esp_h264_dec_sw.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_MAX_DIMENSIONS 2048
#define STBI_MALLOC(n) heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_REALLOC(p, n) heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define STBI_FREE(p) heap_caps_free(p)
#define STB_IMAGE_IMPLEMENTATION
#include "vendor/stb_image.h"

#define MAX_IMAGE_PIXELS (1024 * 1024)
#define MAX_VIDEO_PIXELS (640 * 480)
#define MAX_SAMPLE (128 * 1024)

mediaType getMediaType(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return UNKNOWN_FILE;
    if (!strcasecmp(ext, ".mp4")) return VIDEO_FILE;
    if (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        return IMAGE_FILE;
    return UNKNOWN_FILE;
}

typedef struct {
    unsigned x, y, width, height;
    uint16_t src_x[SCREEN_WIDTH];
} fit;

static fit fitToScreen(unsigned width, unsigned height)
{
    fit fit = {.width = SCREEN_WIDTH, .height = SCREEN_HEIGHT};
    if (width * SCREEN_HEIGHT > height * SCREEN_WIDTH)
        fit.height = (height * SCREEN_WIDTH + width / 2) / width;
    else
        fit.width = (width * SCREEN_HEIGHT + height / 2) / height;
    if (!fit.width) fit.width = 1;
    if (!fit.height) fit.height = 1;
    fit.x = (SCREEN_WIDTH - fit.width) / 2;
    fit.y = (SCREEN_HEIGHT - fit.height) / 2;
    for (unsigned x = 0; x < fit.width; ++x)
        fit.src_x[x] = ((2 * x + 1) * width) / (2 * fit.width);
    return fit;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

// Copy from demo
const char *media_show_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return "Could not open image.";
    int width, height, channels;
    const char *error = "Invalid PNG or JPEG.";
    if (stbi_info_from_file(file, &width, &height, &channels)) {
        if (width > 2048 || height > 2048 || (uint32_t)width * height > MAX_IMAGE_PIXELS) {
            error = "Image exceeds 1 megapixel or 2048 pixels per side.";
        } else {
            uint8_t *rgba = stbi_load_from_file(file, &width, &height, &channels, STBI_rgb_alpha);
            if (rgba) {
                fit fit = fitToScreen(width, height);
                uint16_t *pixels = board_back_buffer();
                memset(pixels, 0, PANEL_BYTES);
                for (unsigned y = 0; y < fit.height; ++y) {
                    unsigned row = ((2 * y + 1) * height) / (2 * fit.height);
                    const uint8_t *src = rgba + (size_t)row * width * 4;
                    uint16_t *dst = pixels + (fit.y + y) * SCREEN_WIDTH + fit.x;
                    for (unsigned x = 0; x < fit.width; ++x) {
                        const uint8_t *p = src + 4 * fit.src_x[x];
                        unsigned a = p[3];
                        dst[x] = rgb565(p[0] * a / 255, p[1] * a / 255, p[2] * a / 255);
                    }
                }
                stbi_image_free(rgba);
                board_present(pixels);
                error = NULL;
            } else {
                error = "image decode failed";
            }
        }
    }
    fclose(file);
    return error;
}

static unsigned clampBytes(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

// Copy from demo
static void render_i420(const uint8_t *buffer, unsigned stride, unsigned rows, unsigned width, unsigned height)
{
    fit fit = fitToScreen(width, height);
    uint16_t *pixels = board_back_buffer();
    memset(pixels, 0, PANEL_BYTES);
    const uint8_t *u_plane = buffer + stride * rows;
    const uint8_t *v_plane = u_plane + stride * rows / 4;
    for (unsigned y = 0; y < fit.height; ++y) {
        unsigned row = ((2 * y + 1) * height) / (2 * fit.height);
        const uint8_t *luma = buffer + row * stride;
        const uint8_t *u = u_plane + (row / 2) * (stride / 2);
        const uint8_t *v = v_plane + (row / 2) * (stride / 2);
        uint16_t *dst = pixels + (fit.y + y) * SCREEN_WIDTH + fit.x;
        for (unsigned x = 0; x < fit.width; ++x) {
            unsigned col = fit.src_x[x];
            int c = (luma[col] - 16) * 298, d = u[col / 2] - 128, e = v[col / 2] - 128;
            dst[x] = rgb565(clampBytes((c + 409 * e + 128) >> 8),
                            clampBytes((c - 100 * d - 208 * e + 128) >> 8),
                            clampBytes((c + 516 * d + 128) >> 8));
        }
    }
}

typedef struct { FILE *file; uint32_t size; } mp4_input_t;
static int getMp4(void *data, uint32_t len, void *ctx)
{
    FILE *file = ((mp4_input_t *)ctx)->file;
    if (usb_disk_requested()) return -1;
    size_t n = fread(data, 1, len, file);
    return ferror(file) ? -1 : (int)n;
}
static int mp4_seek(uint32_t offset, void *ctx)
{
    mp4_input_t *input = ctx;
    return offset <= input->size ? fseek(input->file, offset, SEEK_SET) : -1;
}
static uint32_t mp4_size(void *ctx) { return ((mp4_input_t *)ctx)->size; }

static bool start(const uint8_t *p, size_t n)
{
    return n >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n >= 4 && p[2] == 0 && p[3] == 1));
}

static unsigned setProfile(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 4 < size; ++i)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 31) == 7)
            return data[i + 4];
    return 0;
}

// Copy from demo
typedef struct {
    esp_h264_dec_handle_t decoder;
    esp_h264_dec_param_sw_handle_t params;
    unsigned width, height, fps;
    bool displayed, cancelled;
    media_video_present_t present;
    void *context;
} video_t;

static bool decode(video_t *video, uint8_t *data, size_t size, uint32_t pts)
{
    esp_h264_dec_in_frame_t input = {.raw_data = {.buffer = data, .len = size}, .pts = pts, .dts = pts};
    bool retried = false;
    while (input.raw_data.len) {
        esp_h264_dec_out_frame_t out = {0};
        if (esp_h264_dec_process(video->decoder, &input, &out) != ESP_H264_ERR_OK ||
            input.consume > input.raw_data.len ||
            (!input.consume && (!out.out_size || retried))) return false;
        retried = input.consume == 0;
        if (out.out_size) {
            esp_h264_resolution_t dimensions;
            if (esp_h264_dec_get_resolution(video->params, &dimensions) != ESP_H264_ERR_OK ||
                !dimensions.width || !dimensions.height ||
                (uint32_t)dimensions.width * dimensions.height > MAX_VIDEO_PIXELS ||
                (dimensions.width & 1) || (dimensions.height & 1) ||
                out.out_size < (uint32_t)dimensions.width * dimensions.height * 3 / 2)
                return false;
            unsigned width = video->width < dimensions.width ? video->width : dimensions.width;
            unsigned height = video->height < dimensions.height ? video->height : dimensions.height;
            if (!video->present(out.pts, video->fps, video->context)) {
                video->cancelled = true;
                return true;
            }
            render_i420(out.outbuf, dimensions.width, dimensions.height, width, height);
            board_present(board_back_buffer());
            video->displayed = true;
        }
        input.raw_data.buffer += input.consume;
        input.raw_data.len -= input.consume;
    }
    return true;
}

// Copy from demo
const char *media_play_video(const char *path, media_video_present_t present, void *context)
{
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size <= 0 || info.st_size > INT32_MAX)
        return "MP4 is unreadable";
    mp4_input_t input = {.file = fopen(path, "rb"), .size = info.st_size};
    if (!input.file) return "Could not open MP4.";
    static bool registered;
    const char *error = "MP4 OPEN FAILED";
    if (!registered) registered = esp_mp4_extractor_register() == ESP_EXTRACTOR_ERR_OK;
    esp_extractor_handle_t extractor = NULL;
    esp_extractor_config_t config = {
        .type = ESP_EXTRACTOR_TYPE_MP4, .extract_mask = ESP_EXTRACT_MASK_VIDEO,
        .in_read_cb = getMp4, .in_seek_cb = mp4_seek, .in_size_cb = mp4_size, .in_ctx = &input,
        .out_pool_size = MAX_SAMPLE + 64, .out_align = 16,
    };
    video_t video = {.present = present, .context = context};
    if (!registered || esp_extractor_open(&config, &extractor) != ESP_EXTRACTOR_ERR_OK) goto done;
    setvbuf(input.file, NULL, _IOFBF, 16384);
    bool dynamic = true;
    esp_extractor_ctrl(extractor, ESP_EXTRACTOR_CTRL_TYPE_SET_DYNAMIC_INDEXING, &dynamic, sizeof(dynamic));
    esp_extractor_stream_info_t stream;
    error = "MP4 HEADER ERROR";
    if (esp_extractor_parse_stream(extractor) != ESP_EXTRACTOR_ERR_OK) goto done;
    error = "NEEDS H264 BASELINE";
    if (esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0, &stream) != ESP_EXTRACTOR_ERR_OK ||
        stream.video_info.format != ESP_EXTRACTOR_VIDEO_FORMAT_H264) goto done;
    unsigned profile = 0;
    if (stream.spec_info && stream.spec_info_len >= 4) {
        profile = stream.spec_info[0] == 1 ? stream.spec_info[1] :
            setProfile(stream.spec_info, stream.spec_info_len);
        if (profile && profile != 66) goto done;
    }
    video.width = stream.video_info.width;
    video.height = stream.video_info.height;
    video.fps = stream.video_info.fps;
    if (!video.width || !video.height || video.width > 832 || video.height > 832 ||
        ((video.width + 15) & ~15U) * ((video.height + 15) & ~15U) > MAX_VIDEO_PIXELS) {
        error = "MP4 FRAME TOO LARGE";
        goto done;
    }
    esp_h264_dec_cfg_sw_t decoder_config = {.pic_type = ESP_H264_RAW_FMT_I420};
    error = "H264 INIT FAILED";
    if (esp_h264_dec_sw_new(&decoder_config, &video.decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_open(video.decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_sw_get_param_hd(video.decoder, &video.params) != ESP_H264_ERR_OK) goto done;
    /* The MP4 extractor emits Annex B with SPS/PPS prepended to the first access unit. */
    error = "MP4 HAS NO PICTURES";
    for (;;) {
        esp_extractor_frame_info_t frame = {0};
        esp_extractor_err_t rc = esp_extractor_read_frame(extractor, &frame);
        if (rc == ESP_EXTRACTOR_ERR_EOS) {
            if (video.displayed) error = NULL;
            break;
        }
        if (rc != ESP_EXTRACTOR_ERR_OK) {
            error = "MP4 FAIL";
            break;
        }
        bool valid = true;
        if (frame.stream_type == ESP_EXTRACTOR_STREAM_TYPE_VIDEO && frame.frame_size) {
            unsigned frame_profile = setProfile(frame.frame_buffer, frame.frame_size);
            if (frame_profile && frame_profile != 66) {
                error = "H264 ERROR";
                valid = false;
            } else {
                valid = start(frame.frame_buffer, frame.frame_size) &&
                    decode(&video, frame.frame_buffer, frame.frame_size, frame.pts);
                if (!valid) error = "H264 FAILED";
            }
        }
        bool ended = EXTRACTOR_IS_EOS(frame.frame_flag);
        if (frame.frame_buffer) esp_extractor_release_frame(extractor, &frame);
        if (video.cancelled || (ended && valid && video.displayed)) { error = NULL; break; }
        if (!valid || ended) break;
        vTaskDelay(1);
    }
done:
    if (video.decoder) esp_h264_dec_del(video.decoder);
    if (extractor) esp_extractor_close(extractor);
    fclose(input.file);
    return error;
}
