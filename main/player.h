#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    commandNext, commandTogglePause
} playerCommandType;

esp_err_t playerInit(void);
bool playerSend(playerCommandType command);
void playerRun(void);
