#include "usb_api.h"
#include "player.h"
#include "storage_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "lwip/sockets.h"

static httpd_handle_t server;

static esp_err_t status_handler(httpd_req_t *req)
{
    player_status_t s = player_status();
    char result[1024];
    snprintf(result, sizeof(result),
             "{\"api_version\":2,\"usb_api_version\":1,\"settings_storage\":\"nvs\",\"width\":320,\"height\":820,\"index\":%u,\"total\":%u,\"paused\":%s,\"brightness\":%u,\"sleeping\":%s,\"storage_ready\":%s,\"sleep_remaining\":%u,\"overlay_toggle_supported\":true,\"overlay_enabled\":%s,\"overlay_available\":%s,\"max_fps_supported\":true,\"max_fps_enabled\":%s,\"info_overlay_supported\":true,\"info_overlay_enabled\":%s,\"transport\":\"usb\",\"playback_fps\":%u.%02u,\"source_fps\":%u.%02u,\"dropped_frames\":%u,\"battery_mv\":%u,\"battery_percent\":%d,\"material_revision\":%u,\"error\":\"%s\"}",
             s.card_index, s.card_count, s.paused ? "true" : "false", s.brightness,
             s.sleeping ? "true" : "false", s.storage_ready ? "true" : "false", s.sleep_remaining,
             s.overlay_enabled ? "true" : "false", s.overlay_available ? "true" : "false", s.max_fps_enabled ? "true" : "false", s.info_overlay_enabled ? "true" : "false",
             s.playback_fps_x100/100,s.playback_fps_x100%100,s.source_fps_x100/100,s.source_fps_x100%100,s.dropped_frames,s.battery_mv,s.battery_percent,s.material_revision,s.error);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_handler(httpd_req_t *req)
{
    char query[128], cmd[24], value[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "cmd", cmd, sizeof(cmd)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing command.");
    }
    player_command_type_t command;
    int argument = 0;
    if (!strcmp(cmd, "next")) command = COMMAND_NEXT;
    else if (!strcmp(cmd, "previous")) command = COMMAND_PREVIOUS;
    else if (!strcmp(cmd, "pause")) command = COMMAND_PAUSE;
    else if (!strcmp(cmd, "resume")) command = COMMAND_RESUME;
    else if (!strcmp(cmd, "sleep")) command = COMMAND_SLEEP;
    else if (!strcmp(cmd, "wake")) command = COMMAND_WAKE;
    else if (!strcmp(cmd, "deep_sleep")) command = COMMAND_DEEP_SLEEP;
    else if (!strcmp(cmd, "replay")) command = COMMAND_REPLAY;
    else if (!strcmp(cmd, "brightness") || !strcmp(cmd, "select") || !strcmp(cmd, "sleep_timer") || !strcmp(cmd, "overlay") || !strcmp(cmd, "info_overlay") || !strcmp(cmd, "max_fps")) {
        if (httpd_query_key_value(query, "value", value, sizeof(value)) != ESP_OK)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing value.");
        char *end;
        long number = strtol(value, &end, 10);
        int maximum = !strcmp(cmd,"brightness") ? 255 : !strcmp(cmd,"select") ? (int)player_status().card_count-1 : (!strcmp(cmd,"overlay") || !strcmp(cmd,"info_overlay") || !strcmp(cmd,"max_fps")) ? 1 : 86400;
        if (*end || end == value || number < 0 || number > maximum)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Value is outside the allowed range.");
        argument = number;
        if (!strcmp(cmd,"overlay") && !player_status().overlay_available)
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "This material needs a new package with overlay toggle streams.");
        command = !strcmp(cmd,"brightness") ? COMMAND_BRIGHTNESS : !strcmp(cmd,"select") ? COMMAND_SELECT : !strcmp(cmd,"overlay") ? COMMAND_OVERLAY : !strcmp(cmd,"max_fps") ? COMMAND_MAX_FPS : !strcmp(cmd,"info_overlay") ? COMMAND_INFO_OVERLAY : COMMAND_SLEEP_TIMER;
    } else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown command.");
    if (!player_send(command, argument)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "Command queue full. Try again.");
    }
    return httpd_resp_sendstr(req, "OK");
}

esp_err_t usb_api_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* lwIP's only interface is lo0; never expose this adapter externally. */
    struct ifreq loopback = {0};
    strcpy(loopback.ifr_name, "lo0");
    config.if_name = &loopback;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    /* HTTP dispatch, FatFs/SDMMC and socket calls share this task's stack. */
    config.stack_size = 12288;
    config.max_uri_handlers = 12;
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/control", .method = HTTP_POST, .handler = control_handler}
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    ESP_ERROR_CHECK(storage_api_register(server));
    ESP_LOGI("usb-api", "USB-only loopback API ready");
    return ESP_OK;
}
