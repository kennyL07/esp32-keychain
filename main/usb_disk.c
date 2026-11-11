#include "usb_disk.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"

static bool mounted;

esp_err_t usbDiskInit(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = 1;
    slot.cmd = 2;
    slot.d0 = 42;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    esp_vfs_fat_sdmmc_mount_config_t options = {
        .format_if_mount_failed = false,
        .max_files = 5,
    };
    sdmmc_card_t *card;
    mounted = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &options, &card) == ESP_OK;
    return ESP_OK;
}

bool usbDiskRequested(void) { return false; }
void usbDiskPlayerIdle(void) {}
bool usbDiskLocal(void) { return mounted; }
const char *usbDiskError(void) { return "CHECK FAT32 CARD"; }
