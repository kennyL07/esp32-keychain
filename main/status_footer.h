#pragma once
#include <stdbool.h>
#include <stdint.h>

#define STATUS_FOOTER_HEIGHT 26
typedef struct {
    bool enabled, wifi_enabled;
    unsigned wifi_clients, fps_x100;
    int battery_percent;
} status_footer_t;

/* Paint only the bottom 26 rows. Disabled leaves the supplied artwork untouched. */
void status_footer_draw(uint16_t *pixels, unsigned width, unsigned height, const status_footer_t *status);
