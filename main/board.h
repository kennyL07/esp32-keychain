#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 820
#define PANEL_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT * 2)
#define CARD_ROOT "/sdcard/anims"

esp_err_t board_display_init(void);
void board_present(const uint16_t *pixels);
uint16_t *board_back_buffer(void);
void board_setStatus(const char *line1, const char *line2, const char *line3);
void board_setBrightness(unsigned value);
void board_infoOverlay(bool enabled, unsigned fps_x100, bool maximum_fps);
void batteryInit(void);
bool getBattery(unsigned *millivolts, int *percent);
void setBatteryGliph(int percent);
void buttonInit(void);
bool ifButtonDown(void);
