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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#define maxCards 128
#define maxPath 537
#define maxJpeg (1024 * 1024)
static char (*cards)[maxPath];
static jpeg_dec_handle_t decoder;
static QueueHandle_t commands;
static unsigned selectedIndex, count, generation;
static bool paused;
static int64_t pausedTime;

bool playerSend(playerCommandType command)
{
    return commands && xQueueSend(commands, &command, 0) == pdTRUE;
}

static void processCommands(void)
{
    playerCommandType command;
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
        if (count && command == commandNext) {
            selectedIndex = (selectedIndex + 1) % count;
            ++generation;
        } else if (count && command == commandTogglePause) {
            paused = !paused;
        }
    }
}
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
    commands = xQueueCreate(16, sizeof(playerCommandType));
    if (!commands || !cards || !jpegBuffer || !pixelBuffer) return ESP_ERR_NO_MEM;
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

static bool waitForFrame(int64_t *due, unsigned expectedGeneration)
{
    for (;;) {
        int64_t before = esp_timer_get_time();
        processCommands();
        if (generation != expectedGeneration) return false;
        bool waitingForResume = paused;
        if (!waitingForResume && before >= *due) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if (waitingForResume) { int64_t delay = esp_timer_get_time() - before; *due += delay; pausedTime += delay; }
    }
}

static int playStream(const char *path, unsigned selectedGeneration)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "EPV1", 4) || le16(header + 4) != panelWidth ||
        le16(header + 6) != panelHeight || !le16(header + 8) || !le16(header + 10))
        goto invalid;
    uint32_t frames = le32(header + 12);
    int64_t period = 1000000LL * le16(header + 10) / le16(header + 8);
    int64_t due = esp_timer_get_time();
    for (uint32_t i = 0; i < frames; ++i) {
        if (!waitForFrame(&due, selectedGeneration)) { fclose(file); return 1; }
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
        due += period;
        vTaskDelay(1);
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
    count = scanCards();
    ESP_LOGI("player", "catalog has %u entries", count);
    if (!count) {
        boardStatus("NO MEDIA", "", "");
        return;
    }
    char path[maxPath + 24];
    for (;;) {
        unsigned selectedGeneration = generation;
        snprintf(path, sizeof(path), "%s/loop.epv", cards[selectedIndex]);
        if (playStream(path, selectedGeneration) < 0) {
            boardStatus("MEDIA ERROR", "", "");
            while (generation == selectedGeneration) {
                processCommands();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
}
