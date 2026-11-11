#include "board.h"
#include "player.h"
#include "usb_disk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void buttonTask(void *arg)
{
    bool previous = false, stable = false;
    unsigned debounce = 0, pressedMs = 0, releasedMs = 0, clicks = 0;
    for (;;) {
        bool down = boardButtonDown();
        debounce = down == previous ? debounce + 10 : 0;
        previous = down;
        if (debounce >= 30 && stable != down) {
            stable = down;
            if (down) pressedMs = 0;
            else if (pressedMs < 750) { ++clicks; releasedMs = 0; }
            else clicks = 0;
        }
        if (stable) {
            if (pressedMs < 750) pressedMs += 10;
        } else if (clicks) {
            releasedMs += 10;
            if (releasedMs >= 300) {
                playerSend(clicks == 2 ? commandTogglePause : commandNext);
                clicks = 0;
            }
        }
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
