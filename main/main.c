#include "board.h"
#include "esp_log.h"

void app_main(void)
{
    ESP_ERROR_CHECK(boardDisplayInit());
    boardBrightness(180);

    uint16_t *pixels = boardBackBuffer();
    for (unsigned y = 0; y < panelHeight; ++y)
        for (unsigned x = 0; x < panelWidth; ++x)
            pixels[y * panelWidth + x] = x < 107 ? 0xf800 : x < 214 ? 0x07e0 : 0x001f;
    boardPresent(pixels);
    ESP_LOGI("startup", "panel initialized");
}
