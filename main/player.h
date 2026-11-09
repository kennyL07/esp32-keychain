#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    NEXT, PAUSE, MAX_FPS
} player_command_type_t;

esp_err_t player_init(void);
bool player_send(player_command_type_t command);
void player_run(void);
