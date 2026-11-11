#include "board.h"
#include "player.h"
#include "usb_disk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void buttonTask(void *arg)
{
    bool previous = false;
    for (;;) {
        bool down = boardButtonDown();
        if (down && !previous) playerSend(commandNext);
        previous = down;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(boardDisplayInit());
    boardBrightness(180);

    boardButtonInit();
    ESP_ERROR_CHECK(playerInit());
    ESP_ERROR_CHECK(usbDiskInit());
    xTaskCreate(buttonTask, "button", 3072, NULL, 3, NULL);
    playerRun();
}
