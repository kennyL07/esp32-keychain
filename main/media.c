#include "media.h"
#include "board.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
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
                        dst[x] = rgb565(p[0], p[1], p[2]);
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

