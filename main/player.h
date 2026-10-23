#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef enum {
    COMMAND_NEXT, COMMAND_PREVIOUS, COMMAND_PAUSE, COMMAND_RESUME,
    COMMAND_TOGGLE_PAUSE, COMMAND_BRIGHTNESS,
    COMMAND_SELECT, COMMAND_REPLAY, COMMAND_SLEEP, COMMAND_WAKE,
    COMMAND_SLEEP_TIMER, COMMAND_DEEP_SLEEP, COMMAND_STORAGE_BEGIN, COMMAND_STORAGE_END,
    COMMAND_OVERLAY, COMMAND_INFO_OVERLAY, COMMAND_MAX_FPS
} player_command_type_t;
typedef struct { player_command_type_t type; int value; } player_command_t;
typedef struct {
    unsigned card_index, card_count, brightness;
    bool paused, sleeping, storage_ready;
    bool overlay_enabled, overlay_available, info_overlay_enabled, max_fps_enabled;
    unsigned sleep_remaining;
    unsigned playback_fps_x100, source_fps_x100, dropped_frames;
    unsigned battery_mv;
    int battery_percent;
    unsigned material_revision;
    char error[96];
} player_status_t;

#define PLAYER_MAX_MATERIALS 128
typedef struct {
    unsigned count, revision;
    uint64_t free_bytes;
    char ids[PLAYER_MAX_MATERIALS][65];
} player_material_index_t;
void player_materials_snapshot(player_material_index_t *out);

esp_err_t player_init(void);
bool player_send(player_command_type_t command, int value);
player_status_t player_status(void);
void player_run(void);
/* Mutation callers must wait until the playback FILE has been closed. */
bool player_storage_begin(void);
void player_storage_end(void);
