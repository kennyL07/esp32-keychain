#include "board.h"
#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_idf_version.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static esp_lcd_panel_handle_t panel;
static uint16_t *framebuffers[2];
static unsigned backBuffer = 1;
static SemaphoreHandle_t frameDone;
static portMUX_TYPE flipLock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool flipPending;
static int displayCore;

static const st7701_lcd_init_cmd_t initCommands[] = {
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xEF, (uint8_t[]){0x08}, 1, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5, 0},
    {0xC0, (uint8_t[]){0xE5, 0x02}, 2, 0},
    {0xC1, (uint8_t[]){0x15, 0x0A}, 2, 0},
    {0xC2, (uint8_t[]){0x07, 0x02}, 2, 0},
    {0xCC, (uint8_t[]){0x10}, 1, 0},
    {0xB0, (uint8_t[]){0x00, 0x08, 0x51, 0x0D, 0xCE, 0x06, 0x00, 0x08, 0x08, 0x24, 0x05, 0xD0, 0x0F, 0x6F, 0x36, 0x1F}, 16, 0},
    {0xB1, (uint8_t[]){0x00, 0x10, 0x4F, 0x0C, 0x11, 0x05, 0x00, 0x07, 0x07, 0x18, 0x02, 0xD3, 0x11, 0x6E, 0x34, 0x1F}, 16, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5, 0},
    {0xB0, (uint8_t[]){0x4D}, 1, 0},
    {0xB1, (uint8_t[]){0x37}, 1, 0},
    {0xB2, (uint8_t[]){0x87}, 1, 0},
    {0xB3, (uint8_t[]){0x80}, 1, 0},
    {0xB5, (uint8_t[]){0x4A}, 1, 0},
    {0xB7, (uint8_t[]){0x85}, 1, 0},
    {0xB8, (uint8_t[]){0x21}, 1, 0},
    {0xB9, (uint8_t[]){0x00, 0x13}, 2, 0},
    {0xC0, (uint8_t[]){0x09}, 1, 0},
    {0xC1, (uint8_t[]){0x78}, 1, 0},
    {0xC2, (uint8_t[]){0x78}, 1, 0},
    {0xD0, (uint8_t[]){0x88}, 1, 0},
    {0xE0, (uint8_t[]){0x80, 0x00, 0x02}, 3, 100},
    {0xE1, (uint8_t[]){0x0F, 0xA0, 0x00, 0x00, 0x10, 0xA0, 0x00, 0x00, 0x00, 0x60, 0x60}, 11, 0},
    {0xE2, (uint8_t[]){0x30, 0x30, 0x60, 0x60, 0x45, 0xA0, 0x00, 0x00, 0x46, 0xA0, 0x00, 0x00, 0x00}, 13, 0},
    {0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE4, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE5, (uint8_t[]){0x0F, 0x4A, 0xA0, 0xA0, 0x11, 0x4A, 0xA0, 0xA0, 0x13, 0x4A, 0xA0, 0xA0, 0x15, 0x4A, 0xA0, 0xA0}, 16, 0},
    {0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4, 0},
    {0xE7, (uint8_t[]){0x44, 0x44}, 2, 0},
    {0xE8, (uint8_t[]){0x10, 0x4A, 0xA0, 0xA0, 0x12, 0x4A, 0xA0, 0xA0, 0x14, 0x4A, 0xA0, 0xA0, 0x16, 0x4A, 0xA0, 0xA0}, 16, 0},
    {0xEB, (uint8_t[]){0x02, 0x00, 0x4E, 0x4E, 0xEE, 0x44, 0x00}, 7, 0},
    {0xED, (uint8_t[]){0xFF, 0xFF, 0x04, 0x56, 0x72, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x27, 0x65, 0x40, 0xFF, 0xFF}, 16, 0},
    {0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x40, 0x3F, 0x64}, 6, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t[]){0x00, 0x0E}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x11, NULL, 0, 120},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5, 0},
    {0xE8, (uint8_t[]){0x00, 0x0C}, 2, 10},
    {0xE8, (uint8_t[]){0x00, 0x00}, 2, 0},
    {0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x29, NULL, 0, 20}};

static bool onFrameComplete(esp_lcd_panel_handle_t unused,
                              const esp_lcd_rgb_panel_event_data_t *event, void *ctx)
{
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&flipLock);
    if (flipPending)
    {
        flipPending = false;
        xSemaphoreGiveFromISR(frameDone, &woken);
    }
    portEXIT_CRITICAL_ISR(&flipLock);
    return woken == pdTRUE;
}

void boardBrightness(unsigned value)
{
    if (value > 255)
        value = 255;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - value);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t boardDisplayInit(void)
{
    displayCore = xPortGetCoreID();
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = LEDC_TIMER_0, .duty_resolution = LEDC_TIMER_8_BIT, .freq_hz = 20000, .clk_cfg = LEDC_AUTO_CLK};
    ledc_channel_config_t light = {
        .gpio_num = 6, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = LEDC_CHANNEL_0, .timer_sel = LEDC_TIMER_0, .duty = 255};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ESP_ERROR_CHECK(ledc_channel_config(&light));
    spi_line_config_t lines = {
        .cs_io_type = IO_TYPE_GPIO, .cs_gpio_num = 0, .scl_io_type = IO_TYPE_GPIO, .scl_gpio_num = 2, .sda_io_type = IO_TYPE_GPIO, .sda_gpio_num = 1};
    esp_lcd_panel_io_3wire_spi_config_t io = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(lines, 0);
    esp_lcd_panel_io_handle_t handle;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_3wire_spi(&io, &handle));
    esp_lcd_rgb_panel_config_t rgb = {0};
    rgb.clk_src = LCD_CLK_SRC_DEFAULT;
    rgb.data_width = 16;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
    rgb.in_color_format = LCD_COLOR_FMT_RGB565;
    rgb.out_color_format = LCD_COLOR_FMT_RGB565;
    rgb.dma_burst_size = 64;
#else
    rgb.bits_per_pixel = 16;
    rgb.psram_trans_align = 64;
#endif
    rgb.num_fbs = 2;
    rgb.bounce_buffer_size_px = panelWidth * 10;
    rgb.flags.fb_in_psram = 1;
    rgb.hsync_gpio_num = 38;
    rgb.vsync_gpio_num = 39;
    rgb.de_gpio_num = 40;
    rgb.pclk_gpio_num = 41;
    rgb.disp_gpio_num = -1;
    const int dataPins[16] = {21, 5, 45, 48, 47, 14, 13, 12, 11, 10, 9, 17, 46, 3, 8, 18};
    for (int i = 0; i < 16; ++i)
        rgb.data_gpio_nums[i] = dataPins[i];
    rgb.timings = (esp_lcd_rgb_timing_t){
        .pclk_hz = 18000000, .h_res = panelWidth, .v_res = panelHeight, .hsync_back_porch = 30, .hsync_front_porch = 30, .hsync_pulse_width = 6, .vsync_back_porch = 20, .vsync_front_porch = 20, .vsync_pulse_width = 40};
    st7701_vendor_config_t vendor = {
        .init_cmds = initCommands,
        .init_cmds_size = sizeof(initCommands) / sizeof(initCommands[0]),
        .rgb_config = &rgb,
        .flags.enable_io_multiplex = 1};
    esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = 16, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(handle, &config, &panel));
    frameDone = xSemaphoreCreateBinary();
    if (!frameDone)
        return ESP_ERR_NO_MEM;
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_frame_buf_complete = onFrameComplete};
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 2,
                                                       (void **)&framebuffers[0], (void **)&framebuffers[1]));
    memset(framebuffers[0], 0, panelBytes);
    memset(framebuffers[1], 0, panelBytes);
    return ESP_OK;
}

static void waitForBackBuffer(void)
{
    configASSERT(xPortGetCoreID() == displayCore);
    while (flipPending)
    {
        if (xSemaphoreTake(frameDone, pdMS_TO_TICKS(250)) != pdTRUE && flipPending)
        {
            ESP_LOGE("board", "timed out");
        }
    }
}

uint16_t *boardBackBuffer(void)
{
    waitForBackBuffer();
    return framebuffers[backBuffer];
}

static void presentBackBuffer(void)
{
    while (xSemaphoreTake(frameDone, 0) == pdTRUE)
    {
    }
    portENTER_CRITICAL(&flipLock);
    flipPending = true;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, panelWidth, panelHeight,
                                              framebuffers[backBuffer]);
    if (err != ESP_OK)
        flipPending = false;
    portEXIT_CRITICAL(&flipLock);
    ESP_ERROR_CHECK(err);
    backBuffer ^= 1;
}

void boardPresent(const uint16_t *pixels)
{
    waitForBackBuffer();
    if (pixels != framebuffers[backBuffer])
        memcpy(framebuffers[backBuffer], pixels, panelBytes);
    presentBackBuffer();
}

static const uint8_t glyphs[][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e}};

static void textAt(const char *message, int x, int y, int scale, uint16_t color)
{
    int length = strlen(message);
    if (x < 0)
        x = 0;
    uint16_t *fb = framebuffers[backBuffer];
    for (int i = 0; i < length; ++i)
    {
        char c = message[i];
        int glyph = c >= 'A' && c <= 'Z' ? c - 'A' : c >= '0' && c <= '9' ? c - '0' + 26
                                                                          : -1;
        static const uint8_t percentGlyph[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
        static const uint8_t unknownGlyph[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
        const uint8_t *shape = glyph >= 0 ? glyphs[glyph] : c == '%' ? percentGlyph
                                                        : c == '?'   ? unknownGlyph
                                                                     : NULL;
        if (!shape)
            continue;
        for (int col = 0; col < 5; ++col)
            for (int row = 0; row < 7; ++row)
                if (shape[col] & (1 << row))
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                        {
                            int xx = x + (i * 6 + col) * scale + dx;
                            int yy = y + row * scale + dy;
                            if (xx >= 0 && yy >= 0 && xx < panelWidth && yy < panelHeight)
                                fb[yy * panelWidth + xx] = color;
                        }
    }
}

static void text(const char *message, int y, int scale, uint16_t color)
{
    textAt(message, (panelWidth - strlen(message) * 6 * scale) / 2, y, scale, color);
}

void boardStatus(const char *a, const char *b, const char *c)
{
    waitForBackBuffer();
    memset(framebuffers[backBuffer], 0, panelBytes);
    text(a, 300, 3, 0x07ff);
    text(b, 370, 2, 0xffff);
    text(c, 410, 2, 0xffff);
    presentBackBuffer();
}

void boardButtonInit(void)
{
    gpio_config_t config = {.pin_bit_mask = 1ULL, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&config));
}

bool boardButtonDown(void) { return gpio_get_level(0) == 0; }
