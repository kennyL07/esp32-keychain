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

#define maxImagePixels (1024 * 1024)
#define maxVideoPixels (640 * 480)
#define maxSample (128 * 1024)

mediaKind mediaKindForPath(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return mediaUnknown;
    if (!strcasecmp(ext, ".mp4")) return mediaVideo;
    if (!strcasecmp(ext, ".png") || !strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg"))
        return mediaImage;
    return mediaUnknown;
}

typedef struct {
    unsigned x, y, width, height;
    uint16_t srcX[panelWidth];
} fit;

static fit fitScreen(unsigned width, unsigned height)
{
    fit fit = {.width = panelWidth, .height = panelHeight};
    if (width * panelHeight > height * panelWidth)
        fit.height = (height * panelWidth + width / 2) / width;
    else
        fit.width = (width * panelHeight + height / 2) / height;
    if (!fit.width) fit.width = 1;
    if (!fit.height) fit.height = 1;
    fit.x = (panelWidth - fit.width) / 2;
    fit.y = (panelHeight - fit.height) / 2;
    for (unsigned x = 0; x < fit.width; ++x)
        fit.srcX[x] = ((2 * x + 1) * width) / (2 * fit.width);
    return fit;
}

static uint16_t rgb565(unsigned r, unsigned g, unsigned b)
{
    return ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
}

const char *mediaShowImage(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return "Could not open image.";
    int width, height, channels;
    const char *error = "Invalid PNG or JPEG.";
    if (stbi_info_from_file(file, &width, &height, &channels)) {
        if (width > 2048 || height > 2048 || (uint32_t)width * height > maxImagePixels) {
            error = "Image exceeds 1 megapixel or 2048 pixels per side.";
        } else {
            uint8_t *rgba = stbi_load_from_file(file, &width, &height, &channels, STBI_rgb_alpha);
            if (rgba) {
                fit fit = fitScreen(width, height);
                uint16_t *pixels = boardBackBuffer();
                memset(pixels, 0, panelBytes);
                for (unsigned y = 0; y < fit.height; ++y) {
                    unsigned row = ((2 * y + 1) * height) / (2 * fit.height);
                    const uint8_t *src = rgba + (size_t)row * width * 4;
                    uint16_t *dst = pixels + (fit.y + y) * panelWidth + fit.x;
                    for (unsigned x = 0; x < fit.width; ++x) {
                        const uint8_t *p = src + 4 * fit.srcX[x];
                        unsigned a = p[3];
                        dst[x] = rgb565(p[0] * a / 255, p[1] * a / 255, p[2] * a / 255);
                    }
                }
                stbi_image_free(rgba);
                boardPresent(pixels);
                error = NULL;
            } else {
                error = "image decode failed";
            }
        }
    }
    fclose(file);
    return error;
}

static unsigned clampByte(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return value;
}

static void renderI420(const uint8_t *buffer, unsigned stride, unsigned rows, unsigned width, unsigned height)
{
    fit fit = fitScreen(width, height);
    uint16_t *pixels = boardBackBuffer();
    memset(pixels, 0, panelBytes);
    const uint8_t *uPlane = buffer + stride * rows;
    const uint8_t *vPlane = uPlane + stride * rows / 4;
    for (unsigned y = 0; y < fit.height; ++y) {
        unsigned row = ((2 * y + 1) * height) / (2 * fit.height);
        const uint8_t *luma = buffer + row * stride;
        const uint8_t *u = uPlane + (row / 2) * (stride / 2);
        const uint8_t *v = vPlane + (row / 2) * (stride / 2);
        uint16_t *dst = pixels + (fit.y + y) * panelWidth + fit.x;
        for (unsigned x = 0; x < fit.width; ++x) {
            unsigned col = fit.srcX[x];
            int c = (luma[col] - 16) * 298, d = u[col / 2] - 128, e = v[col / 2] - 128;
            dst[x] = rgb565(clampByte((c + 409 * e + 128) >> 8),
                            clampByte((c - 100 * d - 208 * e + 128) >> 8),
                            clampByte((c + 516 * d + 128) >> 8));
        }
    }
}

typedef struct { FILE *file; uint32_t size; } mp4Input;
static int mp4Read(void *data, uint32_t len, void *ctx)
{
    FILE *file = ((mp4Input *)ctx)->file;
    if (usbDiskRequested()) return -1;
    size_t n = fread(data, 1, len, file);
    return ferror(file) ? -1 : (int)n;
}
static int mp4Seek(uint32_t offset, void *ctx)
{
    mp4Input *input = ctx;
    return offset <= input->size ? fseek(input->file, offset, SEEK_SET) : -1;
}
static uint32_t mp4Size(void *ctx) { return ((mp4Input *)ctx)->size; }

static bool startCode(const uint8_t *p, size_t n)
{
    return n >= 3 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (n >= 4 && p[2] == 0 && p[3] == 1));
}

static unsigned annexBProfile(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i + 4 < size; ++i)
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            (data[i + 3] & 31) == 7)
            return data[i + 4];
    return 0;
}

typedef struct {
    esp_h264_dec_handle_t decoder;
    esp_h264_dec_param_sw_handle_t params;
    unsigned width, height, fps;
    bool displayed, cancelled;
    mediaVideoPresent present;
    void *context;
} videoDecoder;

static bool decode(videoDecoder *video, uint8_t *data, size_t size, uint32_t pts)
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
                (uint32_t)dimensions.width * dimensions.height > maxVideoPixels ||
                (dimensions.width & 1) || (dimensions.height & 1) ||
                out.out_size < (uint32_t)dimensions.width * dimensions.height * 3 / 2)
                return false;
            unsigned width = video->width < dimensions.width ? video->width : dimensions.width;
            unsigned height = video->height < dimensions.height ? video->height : dimensions.height;
            if (!video->present(out.pts, video->fps, video->context)) {
                video->cancelled = true;
                return true;
            }
            renderI420(out.outbuf, dimensions.width, dimensions.height, width, height);
            boardPresent(boardBackBuffer());
            video->displayed = true;
        }
        input.raw_data.buffer += input.consume;
        input.raw_data.len -= input.consume;
    }
    return true;
}

const char *mediaPlayVideo(const char *path, mediaVideoPresent present, void *context)
{
    struct stat info;
    if (stat(path, &info) != 0 || info.st_size <= 0 || info.st_size > INT32_MAX)
        return "MP4 is unreadable";
    mp4Input input = {.file = fopen(path, "rb"), .size = info.st_size};
    if (!input.file) return "Could not open MP4.";
    static bool registered;
    const char *error = "MP4 OPEN FAILED";
    if (!registered) registered = esp_mp4_extractor_register() == ESP_EXTRACTOR_ERR_OK;
    esp_extractor_handle_t extractor = NULL;
    esp_extractor_config_t config = {
        .type = ESP_EXTRACTOR_TYPE_MP4, .extract_mask = ESP_EXTRACT_MASK_VIDEO,
        .in_read_cb = mp4Read, .in_seek_cb = mp4Seek, .in_size_cb = mp4Size, .in_ctx = &input,
        .out_pool_size = maxSample + 64, .out_align = 16,
    };
    videoDecoder video = {.present = present, .context = context};
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
            annexBProfile(stream.spec_info, stream.spec_info_len);
        if (profile && profile != 66) goto done;
    }
    video.width = stream.video_info.width;
    video.height = stream.video_info.height;
    video.fps = stream.video_info.fps;
    if (!video.width || !video.height || video.width > 832 || video.height > 832 ||
        ((video.width + 15) & ~15U) * ((video.height + 15) & ~15U) > maxVideoPixels) {
        error = "MP4 FRAME TOO LARGE";
        goto done;
    }
    esp_h264_dec_cfg_sw_t decoderConfig = {.pic_type = ESP_H264_RAW_FMT_I420};
    error = "H264 INIT FAILED";
    if (esp_h264_dec_sw_new(&decoderConfig, &video.decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_open(video.decoder) != ESP_H264_ERR_OK ||
        esp_h264_dec_sw_get_param_hd(video.decoder, &video.params) != ESP_H264_ERR_OK) goto done;

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
            unsigned frameProfile = annexBProfile(frame.frame_buffer, frame.frame_size);
            if (frameProfile && frameProfile != 66) {
                error = "H264 ERROR";
                valid = false;
            } else {
                valid = startCode(frame.frame_buffer, frame.frame_size) &&
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
