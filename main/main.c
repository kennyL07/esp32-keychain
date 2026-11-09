/* English-language firmware for Waveshare/Spotpear ESP32-S3-LCD-3.16. */
#include "board.h"
#include "player.h"
#include "usb_disk.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static void button_task(void *arg)
{
    bool previous = false, stable = false;
    unsigned debounce = 0, pressed_ms = 0, released_ms = 0, clicks = 0;
    for (;;) {
        bool down = board_button_down();
        debounce = down == previous ? debounce + 10 : 0;
        previous = down;
        if (debounce >= 30 && stable != down) {
            stable = down;
            if (down) pressed_ms = 0;
            else if (pressed_ms < 750) { ++clicks; released_ms = 0; }
            else clicks = 0; /* Long holds have no action. */
        }
        if (stable) {
            if (pressed_ms < 750) pressed_ms += 10;
        } else if (clicks) {
            released_ms += 10;
            if (released_ms >= 300) {
                player_send(clicks >= 3 ? COMMAND_TOGGLE_MAX_FPS :
                            clicks == 2 ? COMMAND_TOGGLE_PAUSE : COMMAND_NEXT);
                clicks = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI("startup", "Keychain player starting");
    ESP_ERROR_CHECK(board_display_init());
    ESP_LOGI("startup", "started");
    board_button_init();
    ESP_ERROR_CHECK(player_init());
    ESP_ERROR_CHECK(usb_disk_init());
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
    player_run();
}
