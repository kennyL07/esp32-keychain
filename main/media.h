#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { mediaUnknown, mediaImage, mediaVideo, mediaEpv } mediaKind;
mediaKind mediaKindForPath(const char *path);
const char *mediaShowImage(const char *path);

typedef bool (*mediaVideoPresent)(uint32_t ptsMs, unsigned fps, void *context);
const char *mediaPlayVideo(const char *path, mediaVideoPresent present, void *context);
