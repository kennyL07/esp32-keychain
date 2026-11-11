#include "board.h"
#include "player.h"
#include "usb_disk.h"

void app_main(void)
{
    ESP_ERROR_CHECK(boardDisplayInit());
    boardBrightness(180);

    ESP_ERROR_CHECK(playerInit());
    ESP_ERROR_CHECK(usbDiskInit());
    playerRun();
}
