#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { UNKNOWN_FILE, IMAGE_FILE, VIDEO_FILE, MEDIA_EPV } mediaType;
mediaType getMediaType(const char *path);
const char *media_show_image(const char *path);

typedef bool (*media_video_present_t)(uint32_t pts_ms, unsigned fps, void *context);
const char *media_play_video(const char *path, media_video_present_t present, void *context);
