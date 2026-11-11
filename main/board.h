#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define panelWidth 320
#define panelHeight 820
#define panelBytes (panelWidth * panelHeight * 2)
#define cardRoot "/sdcard/anims"

esp_err_t boardDisplayInit(void);
void boardPresent(const uint16_t *pixels);
uint16_t *boardBackBuffer(void);
void boardStatus(const char *line1, const char *line2, const char *line3);
void boardBrightness(unsigned value);
void boardInformationOverlay(bool enabled, unsigned fpsX100, bool maximumFps);
void boardBatteryInit(void);
bool boardBatteryRead(unsigned *millivolts, int *percent);
void boardBatteryIndicator(int percent);
void boardButtonInit(void);
bool boardButtonDown(void);
