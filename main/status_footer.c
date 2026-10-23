#include "status_footer.h"

static void pixel(uint16_t *fb, unsigned width, unsigned height, int x, int y, uint16_t color)
{
    if (x >= 0 && y >= 0 && (unsigned)x < width && (unsigned)y < height) fb[y * width + x] = color;
}

static void label(uint16_t *fb, unsigned width, unsigned height, const char *text, int x, int y, uint16_t color)
{
    static const uint8_t digits[10][5] = {
        {0x3e,0x51,0x49,0x45,0x3e},{0x00,0x42,0x7f,0x40,0x00},
        {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
        {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},
        {0x3c,0x4a,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
        {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1e}
    };
    static const uint8_t f[5] = {0x7f,0x09,0x09,0x09,0x01};
    static const uint8_t p[5] = {0x7f,0x09,0x09,0x09,0x06};
    static const uint8_t s[5] = {0x46,0x49,0x49,0x49,0x31};
    static const uint8_t percent[5] = {0x63,0x13,0x08,0x64,0x63};
    static const uint8_t dot[5] = {0x00,0x60,0x60,0x00,0x00};
    static const uint8_t unknown[5] = {0x02,0x01,0x51,0x09,0x06};
    for (int i = 0; text[i]; ++i) {
        char c = text[i];
        const uint8_t *glyph = c >= '0' && c <= '9' ? digits[c - '0'] :
            c == 'F' ? f : c == 'P' ? p : c == 'S' ? s : c == '%' ? percent : c == '.' ? dot : c == '?' ? unknown : 0;
        if (!glyph) continue;
        for (int col = 0; col < 5; ++col) for (int row = 0; row < 7; ++row)
            if (glyph[col] & (1 << row)) for (int dy = 0; dy < 2; ++dy) for (int dx = 0; dx < 2; ++dx)
                pixel(fb, width, height, x + i * 12 + col * 2 + dx, y + row * 2 + dy, color);
    }
}

static void wireless(uint16_t *fb, unsigned width, unsigned height, int top, bool connected)
{
    uint16_t color = connected ? 0x07e0 : 0xfd20;
    for (int y = 1; y < 25; ++y) for (int x = 5; x < 35; ++x) {
        int dx = x - 20, dy = y - (connected ? 22 : 8);
        int distance = dx * dx + dy * dy;
        int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
        bool ink;
        if (connected) {
            /* Three fan arcs and the connection dot: standard Wi-Fi symbol. */
            ink = (dy < -2 && ax <= -dy && ((distance >= 289 && distance <= 361) ||
                   (distance >= 121 && distance <= 169) || (distance >= 25 && distance <= 49))) || distance <= 3;
        } else {
            /* Broadcast tower with side waves, antenna, legs and a cross brace. */
            ink = distance <= 3 || (ax > ay && ((distance >= 16 && distance <= 36) || (distance >= 64 && distance <= 100)));
            if (y >= 10 && y <= 23 && ax <= 0) ink = true;
            if (y >= 12 && y <= 23 && ax == (y - 12) / 2) ink = true;
            if (y == 20 && ax <= 4) ink = true;
        }
        if (ink) pixel(fb, width, height, x, top + y, color);
    }
}

void status_footer_draw(uint16_t *fb, unsigned width, unsigned height, const status_footer_t *status)
{
    if (!fb || !status || !status->enabled || width < 320 || height < STATUS_FOOTER_HEIGHT) return;
    int top = height - STATUS_FOOTER_HEIGHT;
    for (unsigned y = top; y < height; ++y) for (unsigned x = 0; x < width; ++x) fb[y * width + x] = 0;
    if (status->wifi_enabled) wireless(fb, width, height, top, status->wifi_clients > 0);
    unsigned tenths = status->fps_x100 > 9990 ? 999 : (status->fps_x100 + 5) / 10;
    char fps[10];
    int n = 0;
    if (tenths >= 100) fps[n++] = '0' + tenths / 100;
    fps[n++] = '0' + (tenths / 10) % 10;
    fps[n++] = '.'; fps[n++] = '0' + tenths % 10;
    fps[n++] = ' '; fps[n++] = 'F'; fps[n++] = 'P'; fps[n++] = 'S'; fps[n] = 0;
    label(fb, width, height, fps, 53, top + 6, 0xffff);
    int percent = status->battery_percent > 100 ? 100 : status->battery_percent;
    uint16_t color = percent < 0 ? 0xbdf7 : percent <= 15 ? 0xf800 : 0x07e0;
    for (int y = 5; y < 21; ++y) for (int x = 214; x < 244; ++x)
        if (y < 7 || y > 18 || x < 216 || x > 241 ||
            (percent > 0 && y >= 9 && y < 17 && x >= 218 && x < 218 + (percent * 22 + 99) / 100))
            pixel(fb, width, height, x, top + y, color);
    for (int y = 10; y < 16; ++y) for (int x = 244; x < 248; ++x) pixel(fb, width, height, x, top + y, color);
    char level[5]; n = 0;
    if (percent < 0) level[n++] = '?';
    else {
        if (percent == 100) level[n++] = '1';
        if (percent >= 10) level[n++] = '0' + (percent / 10) % 10;
        level[n++] = '0' + percent % 10; level[n++] = '%';
    }
    level[n] = 0;
    label(fb, width, height, level, 256, top + 6, color);
}
