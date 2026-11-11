#pragma once
#include <stdbool.h>
#include <stdint.h>

#define statusFooterHeight 26
typedef struct {
    bool enabled, maxFps;
    unsigned fpsX100;
    int batteryPercent;
} statusFooter;

void statusFooterDraw(uint16_t *pixels, unsigned width, unsigned height, const statusFooter *status);
