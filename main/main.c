#include "board.h"

void app_main(void)
{
    ESP_ERROR_CHECK(boardDisplayInit());
    boardBrightness(180);

}
