#include "player.h"
#include "board.h"
#include "usb_disk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#ifndef CONFIG_KEYCHAIN_BENCHMARK_FRAMES
#define CONFIG_KEYCHAIN_BENCHMARK_FRAMES 0
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#define maxCards 128
#define maxPath 537
#define maxJpeg (1024 * 1024)
static const char *tag = "player";
static char (*cards)[maxPath];
static QueueHandle_t commands;
static struct {
    unsigned cardIndex, cardCount, brightness, playbackFpsX100, sourceFpsX100;
    bool paused, overlayEnabled, infoOverlayEnabled, maxFpsEnabled, hasError;
} state = {.brightness = 180, .overlayEnabled = true, .infoOverlayEnabled = true};
static int64_t batteryDue;
static unsigned generation;
static bool skipIntro;
static jpeg_dec_handle_t decoder;
static uint8_t *jpegBuffer, *pixelBuffer;
static bool handoff;
static int64_t pausedTime;
static unsigned benchmarkRemaining = CONFIG_KEYCHAIN_BENCHMARK_FRAMES;

static void saveMaxFps(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("player", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "max_fps", state.maxFpsEnabled);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
}

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void setError(const char *error)
{
    state.hasError = *error != 0;
    if (*error) ESP_LOGE(tag, "%s", error);
}

bool playerSend(playerCommandType command)
{
    return commands && xQueueSend(commands, &command, 0) == pdTRUE;
}

static void processCommands(void)
{
    if (usbDiskRequested() && !handoff) {
        handoff = true;
        ++generation;
    }
    if (esp_timer_get_time() >= batteryDue) {
        batteryDue = esp_timer_get_time() + 10000000;
        unsigned mv;
        int percent;
        bool valid = boardBatteryRead(&mv, &percent);
        boardBatteryIndicator(valid ? percent : -1);
    }
    playerCommandType command;
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
        if (handoff || !state.cardCount) continue;
        switch (command) {
        case commandNext:
            state.cardIndex = (state.cardIndex + 1) % state.cardCount;
            state.hasError = false;
            skipIntro = false;
            ++generation;
            break;
        case commandTogglePause:
            state.paused = !state.paused;
            break;
        case commandToggleMaxFps:
            state.maxFpsEnabled = !state.maxFpsEnabled;
            skipIntro = true;
            ++generation;
            saveMaxFps();
            break;
        }
    }
    unsigned fps = !state.paused && !handoff && state.cardCount && !state.hasError
        ? state.playbackFpsX100 : 0;
    boardInformationOverlay(state.infoOverlayEnabled, fps, state.maxFpsEnabled);
}

esp_err_t playerInit(void)
{
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t settings;
        if (nvs_open("player", NVS_READONLY, &settings) == ESP_OK) {
            uint8_t enabled;
            if (nvs_get_u8(settings, "overlay", &enabled) == ESP_OK) state.overlayEnabled = enabled != 0;
            if (nvs_get_u8(settings, "info_overlay", &enabled) == ESP_OK) state.infoOverlayEnabled = enabled != 0;
            if (nvs_get_u8(settings, "max_fps", &enabled) == ESP_OK) state.maxFpsEnabled = enabled != 0;
            if (nvs_get_u8(settings, "brightness", &enabled) == ESP_OK) state.brightness = enabled;
            nvs_close(settings);
        }
    }
    boardInformationOverlay(state.infoOverlayEnabled, 0, state.maxFpsEnabled);
    boardBrightness(state.brightness);
    commands = xQueueCreate(16, sizeof(playerCommandType));
    boardBatteryInit();
    cards = heap_caps_calloc(maxCards, sizeof(*cards), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!commands || !cards) return ESP_ERR_NO_MEM;
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
        bool waitingForResume = state.paused;
        if (!waitingForResume && before >= *due) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if (waitingForResume) { int64_t delay = esp_timer_get_time() - before; *due += delay; pausedTime += delay; }
    }
}

typedef struct {
    unsigned slot;
    uint32_t index, size;
    int64_t readUs;
    bool error;
} compressedFrame;

typedef struct {
    FILE *file;
    uint8_t *buffers[2];
    uint32_t frames, maximum;
    QueueHandle_t freeSlots, ready;
    SemaphoreHandle_t done;
    atomic_bool stop;
    bool started;
} frameReader;

static void frameReaderTask(void *arg)
{
    frameReader *r = arg;
    for (uint32_t i = 0; i < r->frames && !atomic_load(&r->stop); ++i) {
        unsigned slot;
        while (xQueueReceive(r->freeSlots, &slot, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (atomic_load(&r->stop)) goto done;
        }
        if (atomic_load(&r->stop)) goto done;
        compressedFrame frame = {.slot = slot, .index = i};
        uint8_t n[4];
        int64_t start = esp_timer_get_time();
        if (fread(n, 1, 4, r->file) != 4) frame.error = true;
        else {
            frame.size = le32(n);
            if (frame.size < 4 || frame.size > r->maximum ||
                fread(r->buffers[slot], 1, frame.size, r->file) != frame.size) frame.error = true;
        }
        frame.readUs = esp_timer_get_time() - start;
        while (xQueueSend(r->ready, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (atomic_load(&r->stop)) goto done;
        }
        if (frame.error) break;
    }
done:
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void frameReaderStop(frameReader *r)
{
    if (r->started) { atomic_store(&r->stop, true); xSemaphoreTake(r->done, portMAX_DELAY); }
    if (r->freeSlots) vQueueDelete(r->freeSlots);
    if (r->ready) vQueueDelete(r->ready);
    if (r->done) vSemaphoreDelete(r->done);
    heap_caps_free(r->buffers[1]);
    memset(r, 0, sizeof(*r));
}

static bool frameReaderStart(frameReader *r, FILE *file, uint32_t frames, uint32_t maximum)
{
    *r = (frameReader){.file = file, .frames = frames, .maximum = maximum};
    atomic_init(&r->stop, false);
    r->buffers[0] = jpegBuffer;
    r->buffers[1] = heap_caps_aligned_alloc(16, maximum, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->freeSlots = xQueueCreate(2, sizeof(unsigned));
    r->ready = xQueueCreate(2, sizeof(compressedFrame));
    r->done = xSemaphoreCreateBinary();
    if (!r->buffers[1] || !r->freeSlots || !r->ready || !r->done) goto failed;
    for (unsigned i = 0; i < 2; ++i) xQueueSend(r->freeSlots, &i, 0);
    if (xTaskCreatePinnedToCore(frameReaderTask, "video_sd", 6144, r, 2, NULL, 1) != pdPASS) goto failed;
    r->started = true;
    return true;
failed:
    frameReaderStop(r);
    return false;
}

// yeah i don't think I could've coded this. wtf is this. ashamed to have v*bed c*ded this
static int playStream(const char *path, unsigned expectedGeneration)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    frameReader reader = {0};
    setvbuf(file, NULL, _IOFBF, 32 * 1024);
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "EPV1", 4) || le16(header + 4) != panelWidth ||
        le16(header + 6) != panelHeight || !le16(header + 8) || !le16(header + 10) ||
        le16(header + 8) > 30 || le16(header + 10) > 1000 || !le32(header + 12) ||
        le32(header + 12) > 216000 || le32(header + 16) > maxJpeg || le32(header + 20)) {
        fclose(file);
        setError("Invalid legacy EPV header.");
        return -1;
    }
    uint32_t frames = le32(header + 12), maxSize = le32(header + 16);
    if (maxSize < 4) { fclose(file); return -1; }
    bool prefetch = frameReaderStart(&reader, file, frames, maxSize);
    int64_t period = 1000000LL * le16(header + 10) / le16(header + 8);
    int64_t due = esp_timer_get_time();
    int64_t firstPresented = 0, firstPaused = pausedTime, lastReport = due;
    int64_t lastYield = due;
    unsigned skipped = 0;
    unsigned displayed = 0;
    int64_t readTotal = 0, decodeTotal = 0, presentTotal = 0, readWaitTotal = 0;
    for (uint32_t i = 0; i < frames; ++i) {
        bool fast = state.maxFpsEnabled || benchmarkRemaining > 0;
        if (fast) due = esp_timer_get_time();

        if (i && !waitForFrame(&due, expectedGeneration)) goto changed;
        uint32_t size;
        uint8_t *compressed = jpegBuffer;
        compressedFrame frame = {0};
        if (prefetch) {
            int64_t waiting = esp_timer_get_time();
            while (xQueueReceive(reader.ready, &frame, pdMS_TO_TICKS(10)) != pdTRUE) {
                processCommands();
                if (generation != expectedGeneration) goto changed;
            }
            readWaitTotal += esp_timer_get_time() - waiting;
            if (frame.error || frame.index != i) goto invalid;
            size = frame.size; compressed = reader.buffers[frame.slot];
            readTotal += frame.readUs;
        } else {
            uint8_t sizeBytes[4];
            if (fread(sizeBytes, 1, 4, file) != 4) goto invalid;
            size = le32(sizeBytes);
        }
        if (size < 4 || size > maxSize || size > maxJpeg) goto invalid;
        if (!fast && esp_timer_get_time() > due + period && i + 1 < frames) {
            if (!prefetch && fseek(file, size, SEEK_CUR) != 0) goto invalid;
            ++skipped;
        } else {
            int64_t stage = esp_timer_get_time();
            if (!prefetch) {
                if (fread(jpegBuffer, 1, size, file) != size) goto invalid;
                readTotal += esp_timer_get_time() - stage;
            }
            stage = esp_timer_get_time();
            jpeg_dec_io_t io = {.inbuf = compressed, .inbuf_len = size, .outbuf = pixelBuffer};
            jpeg_dec_header_info_t info = {0};
            int outputSize = 0;
            if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
                info.width != panelWidth || info.height != panelHeight ||
                jpeg_dec_get_outbuf_len(decoder, &outputSize) != JPEG_ERR_OK ||
                outputSize > maxJpeg || outputSize < panelBytes) goto invalid;

            io.outbuf = outputSize == panelBytes ? (uint8_t *)boardBackBuffer() : pixelBuffer;
            if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) goto invalid;
            decodeTotal += esp_timer_get_time() - stage;
            stage = esp_timer_get_time();
            boardPresent((const uint16_t *)io.outbuf);
            if (!firstPresented) { firstPresented = esp_timer_get_time(); firstPaused = pausedTime; }
            presentTotal += esp_timer_get_time() - stage;
            ++displayed;
        }
        if (prefetch) xQueueSend(reader.freeSlots, &frame.slot, portMAX_DELAY);
        due += period;
        if (fast) {
            if (benchmarkRemaining) --benchmarkRemaining;
            due = esp_timer_get_time();
        }
        int64_t now = esp_timer_get_time();
        if (now-lastReport >= 2000000 || i+1 == frames) {
            int64_t active = firstPresented ? now-firstPresented-(pausedTime-firstPaused) : 0;
            state.playbackFpsX100 = active > 0 && displayed > 1 ? (unsigned)((displayed-1)*100000000ULL/active) : 0;
            state.sourceFpsX100 = le16(header+8)*100/le16(header+10);
            lastReport = now;
        }

        if (!fast || now - lastYield >= 100000) {
            vTaskDelay(1);
            lastYield = esp_timer_get_time();
        }
    }
    if (!waitForFrame(&due, expectedGeneration)) goto changed;
    frameReaderStop(&reader);
    fclose(file);
    return 0;
changed:
    frameReaderStop(&reader);
    fclose(file); return 1;
invalid:
    frameReaderStop(&reader);
    fclose(file);
    setError("Invalid legacy video frame or SD read error.");
    return -1;
}

static void waitUntilChanged(unsigned selectedGeneration)
{
    while (generation == selectedGeneration) {
        vTaskDelay(pdMS_TO_TICKS(20));
        processCommands();
    }
}

void playerRun(void)
{
    unsigned count = 0;
    bool indexed = false, showedUsb = false, showedError = false;
    for (;;) {
        processCommands();
        if (usbDiskRequested()) {
            usbDiskPlayerIdle();
            if (!showedUsb) {
                indexed = false;
                count = 0;
                state.cardCount = 0;
                state.playbackFpsX100 = state.sourceFpsX100 = 0;
                setError("");
                boardStatus("UPLOAD", "", "");
                showedUsb = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        handoff = false;
        showedUsb = false;
        if (!usbDiskLocal()) {
            if (!showedError) {
                boardStatus("SD CARD ERROR", usbDiskError(), "");
                showedError = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        showedError = false;
        if (!indexed) {
            count = scanCards();
            if (usbDiskRequested()) continue;
            indexed = true;
            state.cardCount = count;
            state.cardIndex = 0;
            state.paused = false;
            skipIntro = false;
            if (!count) boardStatus("NO MEDIA", "", "");
        }
        if (!count) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        unsigned selected = state.cardIndex, selectedGeneration = generation;
        jpegBuffer = heap_caps_aligned_alloc(16, maxJpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pixelBuffer = heap_caps_aligned_alloc(16, maxJpeg, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpegBuffer || !pixelBuffer) {
            free(jpegBuffer);
            free(pixelBuffer);
            boardStatus("MEDIA ERROR", "NO MEMORY", "");
            return;
        }
        char path[maxPath + 24];
        snprintf(path, sizeof(path), "%s/loop.epv", cards[selected]);
        int result = playStream(path, selectedGeneration);
        free(jpegBuffer);
        free(pixelBuffer);
        if (result < 0) {
            boardStatus("MEDIA ERROR", "", "");
            while (generation == selectedGeneration) {
                processCommands();
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
    }
}
