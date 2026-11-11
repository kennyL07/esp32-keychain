#pragma once

typedef enum { mediaUnknown, mediaImage, mediaEpv } mediaKind;
mediaKind mediaKindForPath(const char *path);
const char *mediaShowImage(const char *path);
