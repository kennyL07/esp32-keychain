#include "player.h"
#include "board.h"
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
#include "esp_sleep.h"
#include "esp_vfs_fat.h"
#include "driver/rtc_io.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "storage_api.h"
#ifndef CONFIG_KEYCHAIN_BENCHMARK_FRAMES
#define CONFIG_KEYCHAIN_BENCHMARK_FRAMES 0
#endif

#define MAX_CARDS 128
#define MAX_PATH 192
#define MAX_JPEG (1024 * 1024)
static const char *TAG = "player";
static char cards[MAX_CARDS][MAX_PATH];
static QueueHandle_t commands;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static player_status_t state = {.brightness = 180, .overlay_enabled = true, .info_overlay_enabled = true, .battery_percent = -1};
static player_material_index_t material_index;
static SemaphoreHandle_t index_lock;
static int64_t battery_due;
static unsigned generation;
static bool skip_intro;
static jpeg_dec_handle_t decoder;
static uint8_t *jpeg_buffer, *pixel_buffer;
static SemaphoreHandle_t storage_ack;
static bool maintenance, reload_cards;
static int64_t sleep_deadline, deep_sleep_at;
static int64_t paused_time;
static unsigned benchmark_remaining = CONFIG_KEYCHAIN_BENCHMARK_FRAMES;

static void save_settings(const player_status_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("player", NVS_READWRITE, &handle);
    if (err == ESP_OK)
    {
        err = nvs_set_u8(handle, "brightness", settings->brightness);
        if (err == ESP_OK)
            err = nvs_set_u8(handle, "overlay", settings->overlay_enabled);
        if (err == ESP_OK)
            err = nvs_set_u8(handle, "info_overlay", settings->info_overlay_enabled);
        if (err == ESP_OK)
            err = nvs_set_u8(handle, "max_fps", settings->max_fps_enabled);
        if (err == ESP_OK)
            err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK)
        ESP_LOGE(TAG, "Could not save display settings: %s", esp_err_to_name(err));
}

bool player_storage_begin(void)
{
    xSemaphoreTake(storage_ack, 0);
    if (!player_send(COMMAND_STORAGE_BEGIN, 0))
        return false;
    if (xSemaphoreTake(storage_ack, pdMS_TO_TICKS(8000)) == pdTRUE)
        return true;
    player_send(COMMAND_STORAGE_END, 0);
    return false;
}

void player_storage_end(void)
{
    /* The queue is normally empty; never leave playback in maintenance mode. */
    player_command_t msg = {.type = COMMAND_STORAGE_END};
    xQueueSend(commands, &msg, portMAX_DELAY);
}

static uint16_t le16(const uint8_t *p) { return p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

player_status_t player_status(void)
{
    portENTER_CRITICAL(&status_lock);
    player_status_t result = state;
    portEXIT_CRITICAL(&status_lock);
    return result;
}

static void set_error(const char *error)
{
    portENTER_CRITICAL(&status_lock);
    snprintf(state.error, sizeof(state.error), "%s", error);
    portEXIT_CRITICAL(&status_lock);
    if (*error)
        ESP_LOGE(TAG, "%s", error);
}

bool player_send(player_command_type_t command, int value)
{
    player_command_t msg = {.type = command, .value = value};
    return commands && xQueueSend(commands, &msg, 0) == pdTRUE;
}

static void process_commands(void)
{
    if (esp_timer_get_time() >= battery_due)
    {
        bool first = battery_due == 0;
        battery_due = esp_timer_get_time() + 10000000;
        unsigned mv;
        int percent;
        bool valid = board_battery_read(&mv, &percent);
        if (first)
            ESP_LOGI(TAG, "Battery sample: %u mV, estimated %d percent, valid=%s", mv, percent, valid ? "yes" : "no");
        portENTER_CRITICAL(&status_lock);
        state.battery_mv = mv;
        state.battery_percent = valid ? percent : -1;
        portEXIT_CRITICAL(&status_lock);
        board_battery_indicator(valid ? percent : -1);
    }
    if (sleep_deadline && esp_timer_get_time() >= sleep_deadline)
    {
        sleep_deadline = 0;
        player_send(COMMAND_SLEEP, 0);
    }
    if (deep_sleep_at && esp_timer_get_time() >= deep_sleep_at && !board_button_down())
    {
        board_brightness(0);
        rtc_gpio_pullup_en(GPIO_NUM_0);
        rtc_gpio_pulldown_dis(GPIO_NUM_0);
        ESP_ERROR_CHECK(esp_sleep_enable_ext1_wakeup_io(1ULL, ESP_EXT1_WAKEUP_ANY_LOW));
        esp_deep_sleep_start();
    }
    player_command_t cmd;
    while (xQueueReceive(commands, &cmd, 0) == pdTRUE)
    {
        player_status_t copy = player_status();
        switch (cmd.type)
        {
        case COMMAND_NEXT:
        case COMMAND_PREVIOUS:
            if (copy.card_count)
            {
                int delta = cmd.type == COMMAND_NEXT ? 1 : -1;
                copy.card_index = (copy.card_index + copy.card_count + delta) % copy.card_count;
                copy.error[0] = 0;
                skip_intro = false;
                ++generation;
            }
            break;
        case COMMAND_PAUSE:
            copy.paused = true;
            break;
        case COMMAND_RESUME:
            copy.paused = false;
            break;
        case COMMAND_TOGGLE_PAUSE:
            copy.paused = !copy.paused;
            break;
        case COMMAND_BRIGHTNESS:
            if (copy.brightness != (unsigned)cmd.value)
            {
                copy.brightness = cmd.value < 0 ? 0 : cmd.value > 255 ? 255
                                                                      : cmd.value;
                save_settings(&copy);
            }
            board_brightness(copy.sleeping ? 0 : copy.brightness);
            break;
        case COMMAND_SELECT:
            if (cmd.value >= 0 && cmd.value < copy.card_count)
            {
                copy.card_index = cmd.value;
                copy.error[0] = 0;
                skip_intro = false;
                ++generation;
            }
            break;
        case COMMAND_REPLAY:
            skip_intro = false;
            ++generation;
            break;
        case COMMAND_OVERLAY:
            if (copy.overlay_available && copy.overlay_enabled != (cmd.value != 0))
            {
                copy.overlay_enabled = cmd.value != 0;
                skip_intro = true;
                ++generation;
                save_settings(&copy);
                ESP_LOGI(TAG, "Material overlay %s", copy.overlay_enabled ? "enabled" : "disabled");
            }
            break;
        case COMMAND_INFO_OVERLAY:
            if (copy.info_overlay_enabled != (cmd.value != 0))
            {
                copy.info_overlay_enabled = cmd.value != 0;
                save_settings(&copy);
            }
            break;
        case COMMAND_MAX_FPS:
            if (copy.max_fps_enabled != (cmd.value != 0))
            {
                copy.max_fps_enabled = cmd.value != 0;
                skip_intro = true;
                ++generation;
                save_settings(&copy);
                ESP_LOGI(TAG, "Maximum FPS %s", copy.max_fps_enabled ? "enabled" : "disabled");
            }
            break;
        case COMMAND_SLEEP:
            copy.sleeping = true;
            sleep_deadline = 0;
            board_brightness(0);
            break;
        case COMMAND_WAKE:
            copy.sleeping = false;
            sleep_deadline = 0;
            board_brightness(copy.brightness);
            break;
        case COMMAND_SLEEP_TIMER:
            sleep_deadline = cmd.value > 0 ? esp_timer_get_time() + (int64_t)cmd.value * 1000000 : 0;
            break;
        case COMMAND_DEEP_SLEEP:
            deep_sleep_at = esp_timer_get_time() + 1000000;
            break;
        case COMMAND_STORAGE_BEGIN:
            maintenance = true;
            ++generation;
            break;
        case COMMAND_STORAGE_END:
            maintenance = false;
            reload_cards = true;
            ++generation;
            break;
        }
        portENTER_CRITICAL(&status_lock);
        state = copy;
        portEXIT_CRITICAL(&status_lock);
    }
    portENTER_CRITICAL(&status_lock);
    state.sleep_remaining = sleep_deadline > 0 ? (unsigned)((sleep_deadline - esp_timer_get_time() + 999999) / 1000000) : 0;
    bool information_enabled = state.info_overlay_enabled;
    bool maximum_fps = state.max_fps_enabled;
    unsigned footer_fps = !state.paused && !state.sleeping && !maintenance && state.storage_ready &&
                                  state.card_count && !state.error[0]
                              ? state.playback_fps_x100
                              : 0;
    portEXIT_CRITICAL(&status_lock);
    board_information_overlay(information_enabled, footer_fps, maximum_fps);
}

esp_err_t player_init(void)
{
    if (nvs_flash_init() == ESP_OK)
    {
        nvs_handle_t settings;
        if (nvs_open("player", NVS_READONLY, &settings) == ESP_OK)
        {
            uint8_t enabled;
            if (nvs_get_u8(settings, "overlay", &enabled) == ESP_OK)
                state.overlay_enabled = enabled != 0;
            if (nvs_get_u8(settings, "info_overlay", &enabled) == ESP_OK)
                state.info_overlay_enabled = enabled != 0;
            if (nvs_get_u8(settings, "max_fps", &enabled) == ESP_OK)
                state.max_fps_enabled = enabled != 0;
            if (nvs_get_u8(settings, "brightness", &enabled) == ESP_OK)
                state.brightness = enabled;
            nvs_close(settings);
        }
    }
    board_information_overlay(state.info_overlay_enabled, 0, state.max_fps_enabled);
    board_brightness(state.brightness);
    commands = xQueueCreate(16, sizeof(player_command_t));
    storage_ack = xSemaphoreCreateBinary();
    index_lock = xSemaphoreCreateMutex();
    board_battery_init();
    jpeg_buffer = heap_caps_aligned_alloc(16, MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    pixel_buffer = heap_caps_aligned_alloc(16, MAX_JPEG, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!commands || !storage_ack || !index_lock || !jpeg_buffer || !pixel_buffer)
        return ESP_ERR_NO_MEM;
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    return jpeg_dec_open(&config, &decoder) == JPEG_ERR_OK ? ESP_OK : ESP_FAIL;
}

static int compare_cards(const void *a, const void *b) { return strcmp(a, b); }

void player_materials_snapshot(player_material_index_t *out)
{
    xSemaphoreTake(index_lock, portMAX_DELAY);
    memcpy(out, &material_index, sizeof(*out));
    xSemaphoreGive(index_lock);
}

static void publish_material_index(unsigned count)
{
    uint64_t total = 0, available = 0;
    esp_vfs_fat_info("/sdcard", &total, &available);
    xSemaphoreTake(index_lock, portMAX_DELAY);
    material_index.count = count;
    material_index.free_bytes = available;
    for (unsigned i = 0; i < count; ++i)
        snprintf(material_index.ids[i], sizeof(material_index.ids[i]), "%.64s", strrchr(cards[i], '/') + 1);
    ++material_index.revision;
    portENTER_CRITICAL(&status_lock);
    state.card_count = count;
    state.material_revision = material_index.revision;
    portEXIT_CRITICAL(&status_lock);
    xSemaphoreGive(index_lock);
    ESP_LOGI(TAG, "Material index revision %u: %u entries, %llu bytes free", material_index.revision, count, (unsigned long long)available);
}

static unsigned scan_cards(void)
{
    unsigned count = 0;
    DIR *dir = opendir(CARD_ROOT);
    if (!dir)
    {
        publish_material_index(0);
        return 0;
    }
    struct dirent *entry;
    while (count < MAX_CARDS && (entry = readdir(dir)) != NULL)
    {
        if (!storage_api_valid_id(entry->d_name))
            continue;
        char path[MAX_PATH];
        int n = snprintf(path, sizeof(path), "%s/%s/loop.epv", CARD_ROOT, entry->d_name);
        if (n < 0 || n >= sizeof(path))
            continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        path[n - 9] = '\0'; /* Remove the already-validated /loop.epv suffix. */
        memcpy(cards[count], path, (size_t)n - 8);
        ++count;
    }
    closedir(dir);
    qsort(cards, count, sizeof(cards[0]), compare_cards);
    publish_material_index(count);
    ESP_LOGI(TAG, "Found %u converted material(s) in " CARD_ROOT, count);
    return count;
}

static bool wait_for_frame(int64_t *due, unsigned expected_generation)
{
    for (;;)
    {
        int64_t before = esp_timer_get_time();
        process_commands();
        if (generation != expected_generation)
            return false;
        player_status_t s = player_status();
        bool paused = s.paused || s.sleeping;
        if (!paused && before >= *due)
            return true;
        vTaskDelay(pdMS_TO_TICKS(5));
        if (paused)
        {
            int64_t delay = esp_timer_get_time() - before;
            *due += delay;
            paused_time += delay;
        }
    }
}

typedef struct
{
    unsigned slot;
    uint32_t index, size;
    int64_t read_us;
    bool error;
} compressed_frame_t;

typedef struct
{
    FILE *file;
    uint8_t *buffers[2];
    uint32_t frames, maximum;
    QueueHandle_t free_slots, ready;
    SemaphoreHandle_t done;
    atomic_bool stop;
    bool started;
} frame_reader_t;

void check_heap(void) {
    uint32_t free_heap = esp_get_free_heap_size();
    
    ESP_LOGI(TAG, "Free heap: %lu bytes", free_heap);
}

/* Only this worker touches the playback FILE while prefetching. The player joins
 * it before closing the FILE or acknowledging an upload/storage mutation. */
static void frame_reader_task(void *arg)
{
    frame_reader_t *r = arg;
    for (uint32_t i = 0; i < r->frames && !atomic_load(&r->stop); ++i)
    {
        unsigned slot;
        while (xQueueReceive(r->free_slots, &slot, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            if (atomic_load(&r->stop))
                goto done;
        }
        if (atomic_load(&r->stop))
            goto done;
        compressed_frame_t frame = {.slot = slot, .index = i};
        uint8_t n[4];
        int64_t start = esp_timer_get_time();
        if (fread(n, 1, 4, r->file) != 4)
            frame.error = true;
        else
        {
            frame.size = le32(n);
            if (frame.size < 4 || frame.size > r->maximum ||
                fread(r->buffers[slot], 1, frame.size, r->file) != frame.size)
                frame.error = true;
        }
        frame.read_us = esp_timer_get_time() - start;
        while (xQueueSend(r->ready, &frame, pdMS_TO_TICKS(20)) != pdTRUE)
        {
            if (atomic_load(&r->stop))
                goto done;
        }
        if (frame.error)
            break;
    }
done:
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void frame_reader_stop(frame_reader_t *r)
{
    if (r->started)
    {
        atomic_store(&r->stop, true);
        xSemaphoreTake(r->done, portMAX_DELAY);
    }
    if (r->free_slots)
        vQueueDelete(r->free_slots);
    if (r->ready)
        vQueueDelete(r->ready);
    if (r->done)
        vSemaphoreDelete(r->done);
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
    if (!r->buffers[1] || !r->free_slots || !r->ready || !r->done)
        goto failed;
    for (unsigned i = 0; i < 2; ++i)
        xQueueSend(r->free_slots, &i, 0);
    if (xTaskCreatePinnedToCore(frame_reader_task, "video_sd", 6144, r, 2, NULL, 1) != pdPASS)
        goto failed;
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
    if (!file)
        return -1;
    frame_reader_t reader = {0};
    setvbuf(file, NULL, _IOFBF, 32 * 1024);
    uint8_t header[24];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "EPV1", 4) || le16(header + 4) != PANEL_WIDTH ||
        le16(header + 6) != PANEL_HEIGHT || !le16(header + 8) || !le16(header + 10) ||
        le16(header + 8) > 30 || le16(header + 10) > 1000 || !le32(header + 12) ||
        le32(header + 12) > 216000 || le32(header + 16) > MAX_JPEG || le32(header + 20))
    {
        fclose(file);
        set_error("Invalid EPV header. Reconvert this material.");
        return -1;
    }
    uint32_t frames = le32(header + 12), max_size = le32(header + 16);
    if (max_size < 4)
    {
        fclose(file);
        return -1;
    }
    bool prefetch = frame_reader_start(&reader, file, frames, max_size);
    int64_t period = 1000000LL * le16(header + 10) / le16(header + 8);
    int64_t due = esp_timer_get_time();
    int64_t first_presented = 0, first_paused = paused_time, last_report = due;
    int64_t last_yield = due;
    unsigned skipped = 0;
    unsigned displayed = 0;
    int64_t read_total = 0, decode_total = 0, present_total = 0, read_wait_total = 0;
    ESP_LOGI(TAG, "Playing %s (%lu frames)%s", path, (unsigned long)frames, benchmark_remaining ? " [throughput test]" : "");
    for (uint32_t i = 0; i < frames; ++i)
    {
        bool fast = player_status().max_fps_enabled || benchmark_remaining > 0;
        if (fast)
            due = esp_timer_get_time();
        /* Draw the newly selected first frame even when paused, then hold it. */
        if (i && !wait_for_frame(&due, expected_generation))
            goto changed;
        uint32_t size;
        uint8_t *compressed = jpeg_buffer;
        compressed_frame_t frame = {0};
        if (prefetch)
        {
            int64_t waiting = esp_timer_get_time();
            while (xQueueReceive(reader.ready, &frame, pdMS_TO_TICKS(10)) != pdTRUE)
            {
                process_commands();
                if (generation != expected_generation)
                    goto changed;
            }
            read_wait_total += esp_timer_get_time() - waiting;
            if (frame.error || frame.index != i)
                goto invalid;
            size = frame.size;
            compressed = reader.buffers[frame.slot];
            read_total += frame.read_us;
        }
        else
        {
            uint8_t size_bytes[4];
            if (fread(size_bytes, 1, 4, file) != 4)
                goto invalid;
            size = le32(size_bytes);
        }
        if (size < 4 || size > max_size || size > MAX_JPEG)
            goto invalid;
        if (!fast && esp_timer_get_time() > due + period && i + 1 < frames)
        {
            if (!prefetch && fseek(file, size, SEEK_CUR) != 0)
                goto invalid;
            ++skipped;
        }
        else
        {
            int64_t stage = esp_timer_get_time();
            if (!prefetch)
            {
                if (fread(jpeg_buffer, 1, size, file) != size)
                    goto invalid;
                read_total += esp_timer_get_time() - stage;
            }
            stage = esp_timer_get_time();
            jpeg_dec_io_t io = {.inbuf = compressed, .inbuf_len = size, .outbuf = pixel_buffer};
            jpeg_dec_header_info_t info = {0};
            int output_size = 0;
            if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK ||
                info.width != PANEL_WIDTH || info.height != PANEL_HEIGHT ||
                jpeg_dec_get_outbuf_len(decoder, &output_size) != JPEG_ERR_OK ||
                output_size > MAX_JPEG || output_size < PANEL_BYTES)
                goto invalid;
            /* Decode straight into the inactive LCD buffer when no padding is required. */
            io.outbuf = output_size == PANEL_BYTES ? (uint8_t *)board_back_buffer() : pixel_buffer;
            if (jpeg_dec_process(decoder, &io) != JPEG_ERR_OK)
                goto invalid;
            decode_total += esp_timer_get_time() - stage;
            stage = esp_timer_get_time();
            board_present((const uint16_t *)io.outbuf);
            if (!first_presented)
            {
                first_presented = esp_timer_get_time();
                first_paused = paused_time;
            }
            present_total += esp_timer_get_time() - stage;
            ++displayed;
        }
        if (prefetch)
            xQueueSend(reader.free_slots, &frame.slot, portMAX_DELAY);
        due += period;
        if (fast)
        {
            if (benchmark_remaining)
                --benchmark_remaining;
            due = esp_timer_get_time();
        }
        int64_t now = esp_timer_get_time();
        if (now - last_report >= 2000000 || i + 1 == frames)
        {
            int64_t active = first_presented ? now - first_presented - (paused_time - first_paused) : 0;
            portENTER_CRITICAL(&status_lock);
            state.playback_fps_x100 = active > 0 && displayed > 1 ? (unsigned)((displayed - 1) * 100000000ULL / active) : 0;
            state.source_fps_x100 = le16(header + 8) * 100 / le16(header + 10);
            state.dropped_frames = skipped;
            portEXIT_CRITICAL(&status_lock);
            if (frames > 100)
                ESP_LOGI(TAG, "Playback %u.%02u/%u.%02u FPS; SD=%luus JPEG=%luus display=%luus; %u dropped",
                         state.playback_fps_x100 / 100, state.playback_fps_x100 % 100, state.source_fps_x100 / 100, state.source_fps_x100 % 100,
                         (unsigned long)(read_total / (displayed ? displayed : 1)), (unsigned long)(decode_total / (displayed ? displayed : 1)),
                         (unsigned long)(present_total / (displayed ? displayed : 1)), skipped);
            last_report = now;
        }
        /* Maximum mode avoids a mandatory delay on every frame. Give idle a
         * real scheduling slot at least every 100 ms for watchdog servicing. */
        if (!fast || now - last_yield >= 100000)
        {
            vTaskDelay(1);
            last_yield = esp_timer_get_time();
        }
    }
    if (!wait_for_frame(&due, expected_generation))
        goto changed;
    frame_reader_stop(&reader);
    fclose(file);
    if (displayed)
        ESP_LOGI(TAG, "Frame work avg: SD=%luus wait=%luus JPEG=%luus display=%luus; %u shown, %u skipped; measured %u.%02u FPS",
                 (unsigned long)(read_total / displayed), (unsigned long)(read_wait_total / displayed), (unsigned long)(decode_total / displayed),
                 (unsigned long)(present_total / displayed), displayed, skipped, state.playback_fps_x100 / 100, state.playback_fps_x100 % 100);
    if (skipped)
        ESP_LOGW(TAG, "Skipped %u late frames. Try conversion at a lower frame rate.", skipped);
    return 0;
changed:
    frame_reader_stop(&reader);
    fclose(file);
    return 1;
invalid:
    frame_reader_stop(&reader);
    fclose(file);
    set_error("Invalid video frame or SD read error. Reconvert or check the SD card.");
    return -1;
}

void player_run(void)
{
    esp_err_t mounted = board_sd_mount();
    unsigned count = mounted == ESP_OK ? scan_cards() : 0;
    portENTER_CRITICAL(&status_lock);
    state.card_count = count;
    state.storage_ready = mounted == ESP_OK;
    portEXIT_CRITICAL(&status_lock);
    if (mounted != ESP_OK)
    {
        set_error("SD card mount failed. Insert a FAT32 card and restart.");
        board_status("SD CARD ERROR", "INSERT FAT32 CARD", "THEN RESTART");
    }
    else if (!count)
    {
        set_error("No converted materials found in /sdcard/epass.");
        board_status("NO MATERIALS", "COPY EPASS FOLDER", "THEN RESTART");
    }
    for (;;)
    {
        process_commands();
        if (maintenance)
        {
            xSemaphoreGive(storage_ack);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (reload_cards)
        {
            reload_cards = false;
            count = mounted == ESP_OK ? scan_cards() : 0;
            portENTER_CRITICAL(&status_lock);
            state.card_count = count;
            state.card_index = 0;
            state.overlay_available = false;
            state.error[0] = 0;
            portEXIT_CRITICAL(&status_lock);
            if (!count)
                board_status("NO MATERIALS", "UPLOAD VIA USB", "OPEN KEYCHAIN STUDIO");
        }
        if (!count)
        {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        unsigned selected = player_status().card_index, selected_generation = generation;
        char path[MAX_PATH + 24];
        char alternate[MAX_PATH + 24];
        struct stat info;
        snprintf(path, sizeof(path), "%s/intro.epv", cards[selected]);
        bool has_intro = stat(path, &info) == 0;
        snprintf(alternate, sizeof(alternate), "%s/intro-clean.epv", cards[selected]);
        bool has_clean_intro = stat(alternate, &info) == 0;
        snprintf(alternate, sizeof(alternate), "%s/loop-clean.epv", cards[selected]);
        bool has_clean = stat(alternate, &info) == 0 && S_ISREG(info.st_mode) && (!has_intro || has_clean_intro);
        portENTER_CRITICAL(&status_lock);
        state.overlay_available = has_clean;
        bool clean = has_clean && !state.overlay_enabled;
        portEXIT_CRITICAL(&status_lock);
        snprintf(path, sizeof(path), "%s/intro%s.epv", cards[selected], clean ? "-clean" : "");
        bool play_intro = !skip_intro;
        skip_intro = false;
        if (play_intro && stat(path, &info) == 0)
        {
            int result = play_stream(path, selected_generation);
            if (result == 1)
                continue;
            if (result < 0)
                ESP_LOGW(TAG, "Skipping invalid optional intro.");
        }
        snprintf(path, sizeof(path), "%s/loop%s.epv", cards[selected], clean ? "-clean" : "");
        while (generation == selected_generation)
        {
            int result = play_stream(path, selected_generation);
            if (result > 0)
                break;
            if (result < 0)
            {
                board_status("PLAYBACK ERROR", "CHECK SERIAL LOG", "TRY ANOTHER CARD");
                int64_t end = esp_timer_get_time() + 2000000;
                while (esp_timer_get_time() < end)
                {
                    process_commands();
                    if (generation != selected_generation)
                        break;
                    vTaskDelay(pdMS_TO_TICKS(20));
                }
                if (generation == selected_generation)
                    player_send(COMMAND_NEXT, 0);
                process_commands();
            }
        }
    }
}
