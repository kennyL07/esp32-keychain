#pragma once
#include <stdbool.h>
#include "esp_http_server.h"
bool storage_api_valid_id(const char *id);
esp_err_t storage_api_register(httpd_handle_t server);
void storage_api_cleanup(void);