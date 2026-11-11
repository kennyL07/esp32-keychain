#include "player.h"
#include "board.h"
#include "usb_disk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define maxCards 128
#define maxPath 537
#define maxJpeg (1024 * 1024)
static char (*cards)[maxPath];
static jpeg_dec_handle_t decoder;
static uint8_t *jpegBuffer, *pixelBuffer;

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

esp_err_t playerInit(void)
{
    cards = heap_caps_calloc(maxCards, sizeof(*cards), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    jpegBuffer = heap_caps_aligned_alloc(16, maxJpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pixelBuffer = heap_caps_aligned_alloc(16, maxJpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!cards || !jpegBuffer || !pixelBuffer) return ESP_ERR_NO_MEM;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK ? ESP_OK : ESP_FAIL;
}

static int compareCards(const void *a, const void *b) { return strcmp(a, b); }

static unsigned scanCards(void)
{
    unsigned count = 0;
    DIR *dir = opendir(cardRoot);
    if (!dir) return count;
    struct dirent *entry;
    while (!usbDiskRequested() && count < maxCards && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[maxPath + 16];
        int n = snprintf(path, sizeof(path), "%s/%s/loop.epv", cardRoot, entry->d_name);
        if (n < 0 || n >= maxPath) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        n -= sizeof("/loop.epv") - 1;
        memcpy(cards[count], path, n);
        cards[count][n] = 0;
        ++count;
    }
    closedir(dir);
    qsort(cards, count, sizeof(*cards), compareCards);
    return count;
}

static int playStream(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "EPV1", 4) || le16(header + 4) != panelWidth ||
        le16(header + 6) != panelHeight || !le16(header + 8) || !le16(header + 10))
        goto invalid;
    uint32_t frames = le32(header + 12);
    unsigned delayMs = 1000 * le16(header + 10) / le16(header + 8);
    for (uint32_t i = 0; i < frames; ++i) {
        uint8_t length[4];
        if (fread(length, 1, sizeof(length), file) != sizeof(length)) goto invalid;
        uint32_t size = le32(length);
        if (size < 4 || size > maxJpeg || fread(jpegBuffer, 1, size, file) != size)
            goto invalid;
        jpeg_dec_io_t io = {.inbuf = jpegBuffer, .inbuf_len = size, .outbuf = pixelBuffer};
        jpeg_dec_header_info_t info = {0};
        int outputSize = 0;
        if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
            info.width != panelWidth || info.height != panelHeight ||
            jpeg_dec_get_outbuf_len(decoder, &outputSize) != JPEG_ERR_OK ||
            outputSize > maxJpeg || outputSize < panelBytes ||
            jpeg_dec_process(decoder, &io) != JPEG_ERR_OK)
            goto invalid;
        boardPresent((const uint16_t *)pixelBuffer);

        vTaskDelay(pdMS_TO_TICKS(delayMs ? delayMs : 1));
    }
    fclose(file);
    return 0;
invalid:
    fclose(file);
    return -1;
}

void playerRun(void)
{
    if (!usbDiskLocal()) {
        boardStatus("SD CARD ERROR", usbDiskError(), "");
        return;
    }
    unsigned count = scanCards();
    ESP_LOGI("player", "catalog has %u entries", count);
    if (!count) {
        boardStatus("NO MEDIA", "", "");
        return;
    }
    char path[maxPath + 24];
    snprintf(path, sizeof(path), "%s/loop.epv", cards[0]);
    ESP_LOGI("player", "trying %s", path);
    while (playStream(path) == 0) {
    }
    boardStatus("MEDIA ERROR", "", "");
}
