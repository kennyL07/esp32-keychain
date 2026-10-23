/* English-language firmware for Waveshare/Spotpear ESP32-S3-LCD-3.16. */
#include "board.h"
#include "player.h"
#include "usb_api.h"
#include "usb_control.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
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
            else clicks = 0;
        }
        if (stable) {
            if (pressed_ms < 750) pressed_ms += 10;
        } else if (clicks) {
            released_ms += 10;
            if (released_ms >= 300) {
                if (player_status().sleeping) player_send(COMMAND_WAKE, 0);
                else player_send(clicks > 1 ? COMMAND_TOGGLE_PAUSE : COMMAND_NEXT, 0);
                clicks = 0;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void app_main(void)
{
    ESP_LOGI("startup", "Keychain API v2 starting");
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1) {
        rtc_gpio_deinit(GPIO_NUM_0);
        board_button_init();
        /* GPIO0 is also panel initialization CS. Release the wake press first. */
        while (board_button_down()) vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_ERROR_CHECK(board_display_init());
    board_status("KEYCHAIN PLAYER", "STARTING", "PLEASE WAIT");
    ESP_LOGI("startup", "Startup screen presented");
    board_button_init();
    ESP_ERROR_CHECK(player_init());
    xTaskCreate(button_task, "button", 3072, NULL, 3, NULL);
    player_run();
}
