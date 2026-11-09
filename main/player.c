#include "player.h"
#include "board.h"
#include "media.h"
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
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#ifndef CONFIG_KEYCHAIN_BENCHMARK_FRAMES
#define CONFIG_KEYCHAIN_BENCHMARK_FRAMES 0
#endif

#define MAX_CARDS 128
#define MAX_PATH 537
#define MAX_JPEG (1024 * 1024)
static const char *TAG = "player";
static char (*cards)[MAX_PATH];
static QueueHandle_t commands;
static struct {
    unsigned card_index, card_count, brightness, playback_fps_x100, source_fps_x100;
    bool paused, overlay_enabled, info_overlay_enabled, max_fps_enabled, has_error;
} state = {.brightness = 180, .overlay_enabled = true, .info_overlay_enabled = true};
static int64_t battery_due;
static unsigned generation;
static bool skip_intro;
static jpeg_dec_handle_t decoder;
static uint8_t *jpeg_buffer, *pixel_buffer;
static bool handoff;
static int64_t paused_time;
static unsigned benchmark_remaining = CONFIG_KEYCHAIN_BENCHMARK_FRAMES;

static void save_max_fps(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("player", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "max_fps", state.max_fps_enabled);
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) ESP_LOGE(TAG, "Could not save maximum FPS mode: %s", esp_err_to_name(err));
}

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void set_error(const char *error)
{
    state.has_error = *error != 0;
    if (*error) ESP_LOGE(TAG, "%s", error);
}

bool player_send(player_command_type_t command)
{
    return commands && xQueueSend(commands, &command, 0) == pdTRUE;
}

static void process_commands(void)
{
    if (usb_disk_requested() && !handoff) {
        handoff = true;
        ++generation;
    }
    if (esp_timer_get_time() >= battery_due) {
        battery_due = esp_timer_get_time() + 10000000;
        unsigned mv;
        int percent;
        bool valid = board_battery_read(&mv, &percent);
        board_battery_indicator(valid ? percent : -1);
    }
    player_command_type_t command;
    while (xQueueReceive(commands, &command, 0) == pdTRUE) {
        if (handoff || !state.card_count) continue;
        switch (command) {
        case NEXT:
            state.card_index = (state.card_index + 1) % state.card_count;
            state.has_error = false;
            skip_intro = false;
            ++generation;
            break;
        case PAUSE:
            state.paused = !state.paused;
            break;
        case MAX_FPS:
            state.max_fps_enabled = !state.max_fps_enabled;
            skip_intro = true;
            ++generation;
            save_max_fps();
            break;
        }
    }
    unsigned fps = !state.paused && !handoff && state.card_count && !state.has_error
        ? state.playback_fps_x100 : 0;
    board_information_overlay(state.info_overlay_enabled, fps, state.max_fps_enabled);
}

esp_err_t player_init(void)
{
    if (nvs_flash_init() == ESP_OK) {
        nvs_handle_t settings;
        if (nvs_open("player", NVS_READONLY, &settings) == ESP_OK) {
            uint8_t enabled;
            if (nvs_get_u8(settings, "overlay", &enabled) == ESP_OK) state.overlay_enabled = enabled != 0;
            if (nvs_get_u8(settings, "info_overlay", &enabled) == ESP_OK) state.info_overlay_enabled = enabled != 0;
            if (nvs_get_u8(settings, "max_fps", &enabled) == ESP_OK) state.max_fps_enabled = enabled != 0;
            if (nvs_get_u8(settings, "brightness", &enabled) == ESP_OK) state.brightness = enabled;
            nvs_close(settings);
        }
    }
    board_information_overlay(state.info_overlay_enabled, 0, state.max_fps_enabled);
    board_brightness(state.brightness);
    commands = xQueueCreate(16, sizeof(player_command_type_t));
    board_battery_init();
    cards = heap_caps_calloc(MAX_CARDS, sizeof(*cards), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!commands || !cards) return ESP_ERR_NO_MEM;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK ? ESP_OK : ESP_FAIL;
}

static int compare_cards(const void *a, const void *b) { return strcmp(a, b); }

static unsigned scan_directory(const char *root, unsigned count, bool legacy)
{
    DIR *dir = opendir(root);
    if (!dir) return count;
    struct dirent *entry;
    while (!usb_disk_requested() && count < MAX_CARDS && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.' || (!legacy && media_kind(entry->d_name) == MEDIA_UNKNOWN)) continue;
        char path[MAX_PATH + 16];
        int n = snprintf(path, sizeof(path), "%s/%s%s", root, entry->d_name, legacy ? "/loop.epv" : "");
        if (n < 0 || n >= MAX_PATH) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        if (legacy) n -= sizeof("/loop.epv") - 1;
        memcpy(cards[count], path, n);
        cards[count][n] = 0;
        ++count;
    }
    closedir(dir);
    return count;
}

static unsigned scan_cards(void)
{
    unsigned count = scan_directory("/sdcard", 0, false);
    count = scan_directory("/sdcard/media", count, false);
    qsort(cards, count, sizeof(*cards), compare_cards);
    unsigned first_legacy = count;
    count = scan_directory(CARD_ROOT, count, true);
    qsort(cards + first_legacy, count - first_legacy, sizeof(*cards), compare_cards);
    ESP_LOGI(TAG, "Found %u media files", count);
    return count;
}

static bool wait_for_frame(int64_t *due, unsigned expected_generation)
{
    for (;;) {
        int64_t before = esp_timer_get_time();
        process_commands();
        if (generation != expected_generation) return false;
        bool paused = state.paused;
        if (!paused && before >= *due) return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if (paused) { int64_t delay = esp_timer_get_time() - before; *due += delay; paused_time += delay; }
    }
}

typedef struct {
    unsigned slot;
    uint32_t index, size;
    int64_t read_us;
    bool error;
} compressed_frame_t;

typedef struct {
    FILE *file;
    uint8_t *buffers[2];
    uint32_t frames, maximum;
    QueueHandle_t free_slots, ready;
    SemaphoreHandle_t done;
    atomic_bool stop;
    bool started;
} frame_reader_t;

/* Only this worker touches the playback FILE while prefetching. The player joins
 * it before closing the FILE or acknowledging an upload/storage mutation. */
static void frame_reader_task(void *arg)
{
    frame_reader_t *r = arg;
    for (uint32_t i = 0; i < r->frames && !atomic_load(&r->stop); ++i) {
        unsigned slot;
        while (xQueueReceive(r->free_slots, &slot, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (atomic_load(&r->stop)) goto done;
        }
        if (atomic_load(&r->stop)) goto done;
        compressed_frame_t frame = {.slot = slot, .index = i};
        uint8_t n[4];
        int64_t start = esp_timer_get_time();
        if (fread(n, 1, 4, r->file) != 4) frame.error = true;
        else {
            frame.size = le32(n);
            if (frame.size < 4 || frame.size > r->maximum ||
                fread(r->buffers[slot], 1, frame.size, r->file) != frame.size) frame.error = true;
        }
        frame.read_us = esp_timer_get_time() - start;
        while (xQueueSend(r->ready, &frame, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (atomic_load(&r->stop)) goto done;
        }
        if (frame.error) break;
    }
done:
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void frame_reader_stop(frame_reader_t *r)
{
    if (r->started) { atomic_store(&r->stop, true); xSemaphoreTake(r->done, portMAX_DELAY); }
    if (r->free_slots) vQueueDelete(r->free_slots);
    if (r->ready) vQueueDelete(r->ready);
    if (r->done) vSemaphoreDelete(r->done);
    heap_caps_free(r->buffers[1]);
    memset(r, 0, sizeof(*r));
}

static bool frame_reader_start(frame_reader_t *r, FILE *file, uint32_t frames, uint32_t maximum)
{
    *r = (frame_reader_t){.file = file, .frames = frames, .maximum = maximum};
    atomic_init(&r->stop, false);
    r->buffers[0] = jpeg_buffer;
    r->buffers[1] = heap_caps_aligned_alloc(16, maximum, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    r->free_slots = xQueueCreate(2, sizeof(unsigned));
    r->ready = xQueueCreate(2, sizeof(compressed_frame_t));
    r->done = xSemaphoreCreateBinary();
    if (!r->buffers[1] || !r->free_slots || !r->ready || !r->done) goto failed;
    for (unsigned i = 0; i < 2; ++i) xQueueSend(r->free_slots, &i, 0);
    if (xTaskCreatePinnedToCore(frame_reader_task, "video_sd", 6144, r, 2, NULL, 1) != pdPASS) goto failed;
    r->started = true;
    return true;
failed:
    frame_reader_stop(r);
    ESP_LOGW(TAG, "SD prefetch unavailable; using sequential reads");
    return false;
}

/* Result: 0 = complete, 1 = selection changed, -1 = invalid or unreadable stream. */
static int play_stream(const char *path, unsigned expected_generation)
{
    FILE *file = fopen(path, "rb");
    if (!file) return -1;
    frame_reader_t reader = {0};
    setvbuf(file, NULL, _IOFBF, 32 * 1024);
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "EPV1", 4) || le16(header + 4) != PANEL_WIDTH ||
        le16(header + 6) != PANEL_HEIGHT || !le16(header + 8) || !le16(header + 10) ||
        le16(header + 8) > 30 || le16(header + 10) > 1000 || !le32(header + 12) ||
        le32(header + 12) > 216000 || le32(header + 16) > MAX_JPEG || le32(header + 20)) {
        fclose(file);
        set_error("Invalid legacy EPV header.");
        return -1;
    }
    uint32_t frames = le32(header + 12), max_size = le32(header + 16);
    if (max_size < 4) { fclose(file); return -1; }
    bool prefetch = frame_reader_start(&reader, file, frames, max_size);
    int64_t period = 1000000LL * le16(header + 10) / le16(header + 8);
    int64_t due = esp_timer_get_time();
    int64_t first_presented = 0, first_paused = paused_time, last_report = due;
    int64_t last_yield = due;
    unsigned skipped = 0;
    unsigned displayed = 0;
    int64_t read_total = 0, decode_total = 0, present_total = 0, read_wait_total = 0;
    ESP_LOGI(TAG, "Playing %s (%lu frames)%s", path, (unsigned long)frames, benchmark_remaining ? " [throughput test]" : "");
    for (uint32_t i = 0; i < frames; ++i) {
        bool fast = state.max_fps_enabled || benchmark_remaining > 0;
        if (fast) due = esp_timer_get_time();
        /* Draw the newly selected first frame even when paused, then hold it. */
        if (i && !wait_for_frame(&due, expected_generation)) goto changed;
        uint32_t size;
        uint8_t *compressed = jpeg_buffer;
        compressed_frame_t frame = {0};
        if (prefetch) {
            int64_t waiting = esp_timer_get_time();
            while (xQueueReceive(reader.ready, &frame, pdMS_TO_TICKS(10)) != pdTRUE) {
                process_commands();
                if (generation != expected_generation) goto changed;
            }
            read_wait_total += esp_timer_get_time() - waiting;
            if (frame.error || frame.index != i) goto invalid;
            size = frame.size; compressed = reader.buffers[frame.slot];
            read_total += frame.read_us;
        } else {
            uint8_t size_bytes[4];
            if (fread(size_bytes, 1, 4, file) != 4) goto invalid;
            size = le32(size_bytes);
        }
        if (size < 4 || size > max_size || size > MAX_JPEG) goto invalid;
        if (!fast && esp_timer_get_time() > due + period && i + 1 < frames) {
            if (!prefetch && fseek(file, size, SEEK_CUR) != 0) goto invalid;
            ++skipped;
        } else {
            int64_t stage = esp_timer_get_time();
            if (!prefetch) {
                if (fread(jpeg_buffer, 1, size, file) != size) goto invalid;
                read_total += esp_timer_get_time() - stage;
            }
            stage = esp_timer_get_time();
            jpeg_dec_io_t io = {.inbuf = compressed, .inbuf_len = size, .outbuf = pixel_buffer};
            jpeg_dec_header_info_t info = {0};
            int output_size = 0;
            if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
                info.width != PANEL_WIDTH || info.height != PANEL_HEIGHT ||
                jpeg_dec_get_outbuf_len(decoder, &output_size) != JPEG_ERR_OK ||
                output_size > MAX_JPEG || output_size < PANEL_BYTES) goto invalid;
            /* Decode straight into the inactive LCD buffer when no padding is required. */
            io.outbuf = output_size == PANEL_BYTES ? (uint8_t *)board_back_buffer() : pixel_buffer;
            if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK) goto invalid;
            decode_total += esp_timer_get_time() - stage;
            stage = esp_timer_get_time();
            board_present((const uint16_t *)io.outbuf);
            if (!first_presented) { first_presented = esp_timer_get_time(); first_paused = paused_time; }
            present_total += esp_timer_get_time() - stage;
            ++displayed;
        }
        if (prefetch) xQueueSend(reader.free_slots, &frame.slot, portMAX_DELAY);
        due += period;
        if (fast) {
            if (benchmark_remaining) --benchmark_remaining;
            due = esp_timer_get_time();
        }
        int64_t now = esp_timer_get_time();
        if (now-last_report >= 2000000 || i+1 == frames) {
            int64_t active = first_presented ? now-first_presented-(paused_time-first_paused) : 0;
            state.playback_fps_x100 = active > 0 && displayed > 1 ? (unsigned)((displayed-1)*100000000ULL/active) : 0;
            state.source_fps_x100 = le16(header+8)*100/le16(header+10);
            if (frames > 100) ESP_LOGI(TAG, "Playback %u.%02u/%u.%02u FPS; SD=%luus JPEG=%luus display=%luus; %u dropped",
                state.playback_fps_x100/100,state.playback_fps_x100%100,state.source_fps_x100/100,state.source_fps_x100%100,
                (unsigned long)(read_total/(displayed?displayed:1)),(unsigned long)(decode_total/(displayed?displayed:1)),
                (unsigned long)(present_total/(displayed?displayed:1)),skipped);
            last_report = now;
        }
        /* Maximum mode avoids a mandatory delay on every frame. Give idle a
         * real scheduling slot at least every 100 ms for watchdog servicing. */
        if (!fast || now - last_yield >= 100000) {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
    }
    if (!wait_for_frame(&due, expected_generation)) goto changed;
    frame_reader_stop(&reader);
    fclose(file);
    if (displayed) ESP_LOGI(TAG, "Frame work avg: SD=%luus wait=%luus JPEG=%luus display=%luus; %u shown, %u skipped; measured %u.%02u FPS",
        (unsigned long)(read_total/displayed),(unsigned long)(read_wait_total/displayed), (unsigned long)(decode_total/displayed),
        (unsigned long)(present_total/displayed), displayed, skipped,state.playback_fps_x100/100,state.playback_fps_x100%100);
    if (skipped) ESP_LOGW(TAG, "Skipped %u late frames", skipped);
    return 0;
changed:
    frame_reader_stop(&reader);
    fclose(file); return 1;
invalid:
    frame_reader_stop(&reader);
    fclose(file);
    set_error("Invalid legacy video frame or SD read error.");
    return -1;
}

typedef struct {
    unsigned generation, frames;
    uint32_t first_pts;
    int64_t due, started, first_paused;
} video_clock_t;

static bool video_frame(uint32_t pts, unsigned fps, void *context)
{
    video_clock_t *clock = context;
    process_commands();
    if (generation != clock->generation) return false;
    int64_t now = esp_timer_get_time();
    if (!clock->frames) {
        clock->first_pts = pts;
        clock->started = now;
        clock->first_paused = paused_time;
        clock->due = now;
    } else {
        int64_t due = state.max_fps_enabled ? now :
            clock->due + (pts >= clock->first_pts ? (int64_t)(pts - clock->first_pts) * 1000 : 0);
        int64_t prior_pause = paused_time;
        if (!wait_for_frame(&due, clock->generation)) return false;
        clock->due += paused_time - prior_pause;
        now = esp_timer_get_time();
    }
    int64_t elapsed = now - clock->started - (paused_time - clock->first_paused);
    if (clock->frames && elapsed > 0)
        state.playback_fps_x100 = clock->frames * 100000000ULL / elapsed;
    state.source_fps_x100 = fps * 100;
    ++clock->frames;
    return true;
}

static void wait_until_changed(unsigned selected_generation)
{
    while (generation == selected_generation) {
        vTaskDelay(pdMS_TO_TICKS(20));
        process_commands();
    }
}

void player_run(void)
{
    unsigned count = 0;
    bool indexed = false, showed_usb = false, showed_error = false;
    for (;;) {
        process_commands();
        if (usb_disk_requested()) {
            usb_disk_player_idle(); /* No open FAT files or prefetch tasks here. */
            if (!showed_usb) {
                indexed = false;
                count = 0;
                state.card_count = 0;
                state.playback_fps_x100 = state.source_fps_x100 = 0;
                set_error("");
                board_status("USB DRIVE", "COPY MEDIA FILES", "EJECT TO PLAY");
                showed_usb = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        handoff = false;
        showed_usb = false;
        if (!usb_disk_local()) {
            if (!showed_error) {
                set_error("SD card mount failed. Insert a FAT32 card and restart.");
                board_status("SD CARD ERROR", "INSERT FAT32 CARD", "THEN RESTART");
                showed_error = true;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        showed_error = false;
        if (!indexed) {
            count = scan_cards();
            if (usb_disk_requested()) continue;
            indexed = true;
            state.card_count = count;
            state.card_index = 0;
            state.paused = false;
            skip_intro = false;
            if (!count) board_status("NO MEDIA", "COPY MP4 PNG JPG", "TO THE SD CARD");
        }
        if (!count) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
        unsigned selected = state.card_index, selected_generation = generation;
        media_kind_t kind = !strncmp(cards[selected], CARD_ROOT "/", sizeof(CARD_ROOT)) ?
            MEDIA_EPV : media_kind(cards[selected]);
        state.playback_fps_x100 = state.source_fps_x100 = 0;
        set_error("");
        if (kind != MEDIA_EPV) {
            const char *error = NULL;
            do {
                ESP_LOGI(TAG, "Opening %s", cards[selected]);
                if (kind == MEDIA_IMAGE) {
                    error = media_show_image(cards[selected]);
                } else {
                    video_clock_t clock = {.generation = selected_generation};
                    error = media_play_video(cards[selected], video_frame, &clock);
                }
                process_commands();
            } while (kind == MEDIA_VIDEO && !error && generation == selected_generation);
            if (generation == selected_generation) {
                if (error) {
                    set_error(error);
                    board_status("MEDIA ERROR", kind == MEDIA_VIDEO ? "CHECK MP4 FORMAT" : "CHECK IMAGE SIZE", "SELECT ANOTHER FILE");
                }
                wait_until_changed(selected_generation);
            }
            continue;
        }

        jpeg_buffer = heap_caps_aligned_alloc(16, MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        pixel_buffer = heap_caps_aligned_alloc(16, MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!jpeg_buffer || !pixel_buffer) {
            free(jpeg_buffer);
            free(pixel_buffer);
            set_error("Not enough memory for legacy EPV playback.");
            board_status("MEDIA ERROR", "LEGACY EPV", "SELECT ANOTHER FILE");
            wait_until_changed(selected_generation);
            continue;
        }
        char path[MAX_PATH + 24];
        char alternate[MAX_PATH + 24];
        struct stat info;
        snprintf(path, sizeof(path), "%s/intro.epv", cards[selected]);
        bool has_intro = stat(path, &info) == 0;
        snprintf(alternate, sizeof(alternate), "%s/intro-clean.epv", cards[selected]);
        bool has_clean_intro = stat(alternate, &info) == 0;
        snprintf(alternate, sizeof(alternate), "%s/loop-clean.epv", cards[selected]);
        bool has_clean = stat(alternate, &info) == 0 && S_ISREG(info.st_mode) && (!has_intro || has_clean_intro);
        bool clean = has_clean && !state.overlay_enabled;
        snprintf(path, sizeof(path), "%s/intro%s.epv", cards[selected], clean ? "-clean" : "");
        bool play_intro = !skip_intro;
        skip_intro = false;
        if (play_intro && stat(path, &info) == 0) {
            int result = play_stream(path, selected_generation);
            if (result < 0) ESP_LOGW(TAG, "Skipping invalid optional intro.");
        }
        snprintf(path, sizeof(path), "%s/loop%s.epv", cards[selected], clean ? "-clean" : "");
        while (generation == selected_generation) {
            int result = play_stream(path, selected_generation);
            if (result > 0) break;
            if (result < 0) {
                board_status("PLAYBACK ERROR", "LEGACY EPV", "SELECT ANOTHER FILE");
                wait_until_changed(selected_generation);
            }
        }
        free(jpeg_buffer);
        free(pixel_buffer);
    }
}
