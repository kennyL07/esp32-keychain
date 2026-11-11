#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define panelWidth 320
#define panelHeight 820
#define panelBytes (panelWidth * panelHeight * 2)

esp_err_t boardDisplayInit(void);
void boardPresent(const uint16_t *pixels);
uint16_t *boardBackBuffer(void);
void boardBrightness(unsigned value);
