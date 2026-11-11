#include "usb_disk.h"
#include <stdio.h>
#include <stdatomic.h>
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb_msc.h"
#include "tinyusb_default_config.h"

static const char *TAG = "usb-disk";
static sdmmc_card_t card;
static SemaphoreHandle_t player_idle;
static atomic_bool requested, local;
static bool has_card;
static char error_text[24] = "CHECK FAT32 CARD";

bool usb_disk_requested(void) { return atomic_load(&requested); }
bool usb_disk_local(void) { return atomic_load(&local); }
const char *getCardError(void) { return error_text; }
void usb_disk_player_idle(void) { if (usb_disk_requested()) xSemaphoreGive(player_idle); }

static void usb_event(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_DETACHED) atomic_store(&requested, false);
}

static void mount_event(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        bool app = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
        atomic_store(&local, app);
        if (app) atomic_store(&requested, false);
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
               event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED) {
        ESP_LOGW(TAG, "SD upload failed (event %d)", event->id);
    }
}

/* TinyUSB's attach path would unmount FAT immediately. Keep its SCSI eject
 * and detach handling, but wait here until playback has closed every file. */
void __real_msc_storage_mount_to_usb(void);
void __wrap_msc_storage_mount_to_usb(void)
{
    if (!has_card) return;
    if (!usb_disk_local()) {
        atomic_store(&requested, true);
        __real_msc_storage_mount_to_usb();
        return;
    }
    xSemaphoreTake(player_idle, 0);
    atomic_store(&requested, true);
    if (xSemaphoreTake(player_idle, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "sd card held");
        atomic_store(&requested, false);
        return;
    }
    __real_msc_storage_mount_to_usb();
    if (usb_disk_local()) atomic_store(&requested, false);
}

esp_err_t usb_disk_init(void)
{
    player_idle = xSemaphoreCreateBinary();
    if (!player_idle) return ESP_ERR_NO_MEM;
    tinyusb_msc_driver_config_t driver = {.callback = mount_event};
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver), TAG, "MSC setup failed");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = 1;
    slot.cmd = 2;
    slot.d0 = 42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(200));
    for (unsigned i = 0; i < 3; ++i) {
        err = sdmmc_host_init();
        if (err != ESP_OK) break;
        err = sdmmc_host_init_slot(host.slot, &slot);
        if (err != ESP_OK) {
            sdmmc_host_deinit();
            break;
        }
        err = sdmmc_card_init(&host, &card);
        if (err == ESP_OK) break;
        sdmmc_host_deinit_slot(host.slot);
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    tinyusb_msc_storage_config_t config = {
        .medium.card = &card,
        .fat_fs = {
            .base_path = "/sdcard",
            .config = {.max_files = 5},
            .do_not_format = true,
        },
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_APP,
    };
    tinyusb_msc_storage_handle_t handle;
    err = tinyusb_msc_new_storage_sdmmc(&config, &handle);
    if (err == ESP_OK && !usb_disk_local()) {
        err = tinyusb_msc_delete_storage(handle);
        if (err == ESP_OK) {
            config.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
            err = tinyusb_msc_new_storage_sdmmc(&config, &handle);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "sd unavailable: %s", esp_err_to_name(err));
        snprintf(error_text, sizeof(error_text), "SD MOUNT 0X%X", (unsigned)err);
        return ESP_OK;
    }
    has_card = true;
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG(usb_event);
    usb.task.size = 8192;
    return tinyusb_driver_install(&usb);
}
