#pragma once
#include <stdbool.h>
#include "esp_err.h"

esp_err_t usbDiskInit(void);
bool usbDiskRequested(void);
bool usbDiskLocal(void);
const char *usbDiskError(void);
void usbDiskPlayerIdle(void);
