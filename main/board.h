#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define PANEL_WIDTH 320
#define PANEL_HEIGHT 820
#define PANEL_BYTES (PANEL_WIDTH * PANEL_HEIGHT * 2)
#define CARD_ROOT "/sdcard/epass"

esp_err_t board_display_init(void);
esp_err_t board_sd_mount(void);
void board_present(const uint16_t *pixels);
/* Only the player task may acquire/write this 16-byte-aligned, inactive LCD buffer. */
uint16_t *board_back_buffer(void);
void board_status(const char *line1, const char *line2, const char *line3);
void board_brightness(unsigned value);
void board_wifi_indicator(bool enabled, unsigned clients);
void board_information_overlay(bool enabled, unsigned fps_x100);
void board_battery_init(void);
bool board_battery_read(unsigned *millivolts, int *percent);
void board_battery_indicator(int percent);
void board_button_init(void);
bool board_button_down(void);
