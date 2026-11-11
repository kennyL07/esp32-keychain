#include "board.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(boardDisplayInit());
    boardBrightness(64);
    ESP_LOGI("startup", "panel initialized");
}
