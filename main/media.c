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
    if(!strcasecmp(ext, ".heic"))
        ELOG_ERROR("heic not supported")
        return mediaUnknown
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
    if (!registered || esp_extractor_open(&config, &extractor) != ESP_EXTRACTOR_ERR_OK) goto done;
    setvbuf(input.file, NULL, _IOFBF, 16384);
    bool dynamic = true;
    esp_extractor_ctrl(extractor, ESP_EXTRACTOR_CTRL_TYPE_SET_DYNAMIC_INDEXING, &dynamic, sizeof(dynamic));
    esp_extractor_stream_info_t stream;
    error = "MP4 HEADER ERROR";
    if (esp_extractor_parse_stream(extractor) != ESP_EXTRACTOR_ERR_OK ||
        esp_extractor_get_stream_info(extractor, ESP_EXTRACTOR_STREAM_TYPE_VIDEO, 0, &stream) != ESP_EXTRACTOR_ERR_OK)
        goto done;
    ESP_LOGI("media", "probe %s: %ux%u @ %u", path, (unsigned)stream.video_info.width,
             (unsigned)stream.video_info.height, (unsigned)stream.video_info.fps);

    error = "MP4 PROBE ONLY";
done:
    if (extractor) esp_extractor_close(extractor);
    fclose(input.file);
    return error;
}
