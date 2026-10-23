#include "usb_control.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_BODY 65536
#define MAX_REPLY 16384
static uint8_t *body;
static char *reply;
static char active_token[17];
static int64_t last_request;
typedef struct { size_t length; bool overflow; } response_t;
static unsigned le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void put16(uint8_t *p, unsigned n) { p[0] = n; p[1] = n >> 8; }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = n >> (8*i); }
static bool read_exact(void *destination, size_t length)
{
    uint8_t *p = destination;
    while (length) {
        int n = usb_serial_jtag_read_bytes(p, length, pdMS_TO_TICKS(5000));
        if (n <= 0) return false;
        p += n; length -= n;
    }
    return true;
}
static bool write_exact(const void *source, size_t length)
{
    const uint8_t *p = source;
    while (length) {
        int n = usb_serial_jtag_write_bytes(p, length, pdMS_TO_TICKS(2000));
        if (n <= 0) return false;
        p += n; length -= n;
    }
    return true;
}
static esp_err_t receive_http(esp_http_client_event_t *event)
{
    response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA) {
        if (response->length + event->data_len >= MAX_REPLY) { response->overflow = true; return ESP_FAIL; }
        memcpy(reply + response->length, event->data, event->data_len);
        response->length += event->data_len;
    }
    return ESP_OK;
}
static int request(const char *path, bool post, const uint8_t *data, size_t length, size_t *received)
{
    char url[448]; snprintf(url, sizeof(url), "http://127.0.0.1%s", path);
    response_t response = {0};
    esp_http_client_config_t config = {.url = url, .method = post ? HTTP_METHOD_POST : HTTP_METHOD_GET,
        .event_handler = receive_http, .user_data = &response, .timeout_ms = 120000,
        .disable_auto_redirect = true, .buffer_size = 2048, .buffer_size_tx = 1024};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { strcpy(reply, "Not enough memory for USB request."); *received = strlen(reply); return 503; }
    esp_http_client_set_header(client, "X-Keychain-Transport", "usb");
    esp_http_client_set_header(client, "Connection", "close");
    if (post) {
        esp_http_client_set_header(client, "Content-Type", "application/octet-stream");
        esp_http_client_set_post_field(client, length ? (const char *)data : "", length);
    }
    esp_err_t err = esp_http_client_perform(client);
    int code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK || response.overflow || code < 100) {
        snprintf(reply, MAX_REPLY, "USB API request failed: %s", response.overflow ? "response too large" : esp_err_to_name(err));
        response.length = strlen(reply); code = 502;
    }
    reply[response.length] = 0; *received = response.length;
    return code;
}
static void track_session(const char *path, int code)
{
    if (code != 200) return;
    if (!strncmp(path, "/api/upload/start?", 18)) {
        char token[17] = {0};
        if (sscanf(reply, "{\"token\":\"%16[0-9a-f]\"}", token) == 1 && strlen(token) == 16)
            memcpy(active_token, token, sizeof(active_token));
    } else if (!strncmp(path, "/api/upload/commit?", 19) || !strncmp(path, "/api/upload/abort?", 18)) active_token[0] = 0;
}
static void usb_task(void *unused)
{
    unsigned matched = 0;
    for (;;) {
        uint8_t ch;
        if (usb_serial_jtag_read_bytes(&ch, 1, pdMS_TO_TICKS(250)) != 1) {
            if (*active_token && (!usb_serial_jtag_is_connected() || esp_timer_get_time() - last_request > 60000000)) {
                char path[64]; size_t length;
                snprintf(path, sizeof(path), "/api/upload/abort?token=%s", active_token);
                request(path, true, NULL, 0, &length); active_token[0] = 0;
            }
            continue;
        }
        if (ch != (uint8_t)"KCH1"[matched]) { matched = ch == 'K' ? 1 : 0; continue; }
        if (++matched < 4) continue;
        matched = 0;
        uint8_t header[16] = {0};
        if (!read_exact(header + 4, 12)) continue;
        uint32_t id = le32(header + 4), length = le32(header + 12);
        unsigned method = le16(header + 8), path_length = le16(header + 10);
        if (method > 1 || path_length < 5 || path_length >= 384 || length > MAX_BODY || (!method && length)) continue;
        char path[384];
        if (!read_exact(path, path_length) || !read_exact(body, length)) continue;
        last_request = esp_timer_get_time();
        path[path_length] = 0;
        bool valid = !memcmp(path, "/api/", 5);
        for (unsigned i = 0; i < path_length; ++i) if (path[i] < '!' || path[i] > '~') valid = false;
        size_t received;
        int code;
        if (valid) { code = request(path, method == 1, body, length, &received); track_session(path, code); }
        else { strcpy(reply, "Invalid API path."); received = strlen(reply); code = 400; }
        memcpy(header, "KCR1", 4); put32(header + 4, id); put16(header + 8, code); put16(header + 10, 0); put32(header + 12, received);
        if (write_exact(header, sizeof(header))) write_exact(reply, received);
        usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
    }
}
esp_err_t usb_control_init(void)
{
    body = heap_caps_malloc(MAX_BODY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    reply = heap_caps_malloc(MAX_REPLY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body || !reply) return ESP_ERR_NO_MEM;
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 8192, .rx_buffer_size = 8192};
    esp_err_t err = usb_serial_jtag_driver_install(&config);
    if (err != ESP_OK) return err;
    return xTaskCreatePinnedToCore(usb_task, "usb-control", 8192, NULL, 4, NULL, 1) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
