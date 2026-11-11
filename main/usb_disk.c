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

static const char *tag = "usb-disk";
static sdmmc_card_t card;
static SemaphoreHandle_t playerIdle;
static atomic_bool requested, local;
static bool hasCard;
static char errorText[24] = "card error";

bool usbDiskRequested(void) { return atomic_load(&requested); }
bool usbDiskLocal(void) { return atomic_load(&local); }
const char *usbDiskError(void) { return errorText; }
void usbDiskPlayerIdle(void) { if (usbDiskRequested()) xSemaphoreGive(playerIdle); }

static void usbEvent(tinyusb_event_t *event, void *arg)
{
    if (event->id == TINYUSB_EVENT_DETACHED) atomic_store(&requested, false);
}

static void mountEvent(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_COMPLETE) {
        bool app = event->mount_point == TINYUSB_MSC_STORAGE_MOUNT_APP;
        atomic_store(&local, app);
        if (app) atomic_store(&requested, false);
    } else if (event->id == TINYUSB_MSC_EVENT_MOUNT_FAILED ||
               event->id == TINYUSB_MSC_EVENT_FORMAT_REQUIRED) {
        ESP_LOGW(tag, "SD upload failed (event %d)", event->id);
    }
}

void __real_msc_storage_mount_to_usb(void);
void __wrap_msc_storage_mount_to_usb(void)
{
    if (!hasCard) return;
    if (!usbDiskLocal()) {
        atomic_store(&requested, true);
        __real_msc_storage_mount_to_usb();
        return;
    }
    xSemaphoreTake(playerIdle, 0);
    atomic_store(&requested, true);
    if (xSemaphoreTake(playerIdle, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(tag, "sd card held");
        atomic_store(&requested, false);
        return;
    }
    __real_msc_storage_mount_to_usb();
    if (usbDiskLocal()) atomic_store(&requested, false);
}

esp_err_t usbDiskInit(void)
{
    playerIdle = xSemaphoreCreateBinary();
    if (!playerIdle) return ESP_ERR_NO_MEM;
    tinyusb_msc_driver_config_t driver = {.callback = mountEvent};
    ESP_RETURN_ON_ERROR(tinyusb_msc_install_driver(&driver), tag, "MSC setup failed");
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = 1;
    slot.cmd = 2;
    slot.d0 = 42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_err_t err = sdmmc_host_init();
    if (err == ESP_OK) err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) err = sdmmc_card_init(&host, &card);
    if (err != ESP_OK) {
        snprintf(errorText, sizeof(errorText), "SD INIT 0X%X", (unsigned)err);
        return ESP_OK;
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
    if (err == ESP_OK && !usbDiskLocal()) {
        err = tinyusb_msc_delete_storage(handle);
        if (err == ESP_OK) {
            config.mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB;
            err = tinyusb_msc_new_storage_sdmmc(&config, &handle);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(tag, "sd unavailable: %s", esp_err_to_name(err));
        snprintf(errorText, sizeof(errorText), "SD MOUNT 0X%X", (unsigned)err);
        return ESP_OK;
    }
    hasCard = true;
    tinyusb_config_t usb = TINYUSB_DEFAULT_CONFIG(usbEvent);
    usb.task.size = 8192;
    return tinyusb_driver_install(&usb);
}
