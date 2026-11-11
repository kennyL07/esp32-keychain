#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usb_disk_init(void);
bool usb_disk_requested(void);
bool usb_disk_local(void);
const char *getCardError(void);
void usb_disk_player_idle(void);
