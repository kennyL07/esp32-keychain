/* Panel commands and pin assignments follow the manufacturer's 3.16-inch demo. */
#include "board.h"
#include "status_footer.h"
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
static unsigned back_buffer = 1;
static SemaphoreHandle_t frame_done;
static portMUX_TYPE flip_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool flip_pending;
static int display_core;
static int battery_percent = -1;
static bool information_enabled = true;
static bool maximum_fps_enabled;
static unsigned footer_fps_x100;
static bool frame_ready;
static uint16_t *footer_background;
static void draw_information_overlay(void);

// Copy from demo
static const st7701_lcd_init_cmd_t init_commands[] = {
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

static bool on_frame_complete(esp_lcd_panel_handle_t unused,
                              const esp_lcd_rgb_panel_event_data_t *event, void *ctx)
{
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&flip_lock);
    if (flip_pending)
    {
        flip_pending = false;
        xSemaphoreGiveFromISR(frame_done, &woken);
    }
    portEXIT_CRITICAL_ISR(&flip_lock);
    return woken == pdTRUE;
}

void board_setBrightness(unsigned value)
{
    if (value > 255)
        value = 255;
    /* This board's backlight input is active-low: zero means fully lit. */
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - value);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t board_display_init(void)
{
    display_core = xPortGetCoreID();
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
    rgb.bounce_buffer_size_px = SCREEN_WIDTH * 10;
    rgb.flags.fb_in_psram = 1;
    rgb.hsync_gpio_num = 38;
    rgb.vsync_gpio_num = 39;
    rgb.de_gpio_num = 40;
    rgb.pclk_gpio_num = 41;
    rgb.disp_gpio_num = -1;
    const int data_pins[16] = {21, 5, 45, 48, 47, 14, 13, 12, 11, 10, 9, 17, 46, 3, 8, 18};
    for (int i = 0; i < 16; ++i)
        rgb.data_gpio_nums[i] = data_pins[i];
    rgb.timings = (esp_lcd_rgb_timing_t){
        .pclk_hz = 18000000, .h_res = SCREEN_WIDTH, .v_res = SCREEN_HEIGHT, .hsync_back_porch = 30, .hsync_front_porch = 30, .hsync_pulse_width = 6, .vsync_back_porch = 20, .vsync_front_porch = 20, .vsync_pulse_width = 40};
    st7701_vendor_config_t vendor = {
        .init_cmds = init_commands,
        .init_cmds_size = sizeof(init_commands) / sizeof(init_commands[0]),
        .rgb_config = &rgb,
        .flags.enable_io_multiplex = 1};
    esp_lcd_panel_dev_config_t config = {
        .reset_gpio_num = 16, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16, .vendor_config = &vendor};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7701(handle, &config, &panel));
    frame_done = xSemaphoreCreateBinary();
    if (!frame_done)
        return ESP_ERR_NO_MEM;
    esp_lcd_rgb_panel_event_callbacks_t callbacks = {.on_frame_buf_complete = on_frame_complete};
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_register_event_callbacks(panel, &callbacks, NULL));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 2,
                                                       (void **)&framebuffers[0], (void **)&framebuffers[1]));
    memset(framebuffers[0], 0, PANEL_BYTES);
    memset(framebuffers[1], 0, PANEL_BYTES);
    footer_background = heap_caps_calloc(SCREEN_WIDTH * 26, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!footer_background)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

static void wait_for_back_buffer(void)
{
    configASSERT(xPortGetCoreID() == display_core);
    while (flip_pending)
    {
        if (xSemaphoreTake(frame_done, pdMS_TO_TICKS(250)) != pdTRUE && flip_pending)
        {
            ESP_LOGE("board", "timed out");
        }
    }
}

uint16_t *board_back_buffer(void)
{
    wait_for_back_buffer();
    return framebuffers[back_buffer];
}

static void present_back_buffer(bool fresh_content)
{
    uint16_t *footer = framebuffers[back_buffer] + (SCREEN_HEIGHT - 26) * SCREEN_WIDTH;
    if (fresh_content)
        memcpy(footer_background, footer, SCREEN_WIDTH * 26 * sizeof(uint16_t));
    else
        memcpy(footer, footer_background, SCREEN_WIDTH * 26 * sizeof(uint16_t));
    draw_information_overlay();
    while (xSemaphoreTake(frame_done, 0) == pdTRUE)
    {
    }
    portENTER_CRITICAL(&flip_lock);
    flip_pending = true;
    esp_err_t err = esp_lcd_panel_draw_bitmap(panel, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT,
                                              framebuffers[back_buffer]);
    if (err != ESP_OK)
        flip_pending = false;
    portEXIT_CRITICAL(&flip_lock);
    ESP_ERROR_CHECK(err);
    back_buffer ^= 1;
    frame_ready = true;
}

void board_infoOverlay(bool enabled, unsigned fps_x100, bool maximum_fps)
{
    bool redraw = information_enabled != enabled || maximum_fps_enabled != maximum_fps ||
                  ((footer_fps_x100 == 0) != (fps_x100 == 0));
    information_enabled = enabled;
    maximum_fps_enabled = maximum_fps;
    footer_fps_x100 = fps_x100;
    if (!redraw || !frame_ready)
        return;
    wait_for_back_buffer();
    memcpy(framebuffers[back_buffer], framebuffers[back_buffer ^ 1], PANEL_BYTES);
    present_back_buffer(false);
}

void board_present(const uint16_t *pixels)
{
    wait_for_back_buffer();
    if (pixels != framebuffers[back_buffer])
        memcpy(framebuffers[back_buffer], pixels, PANEL_BYTES);
    present_back_buffer(true);
}

static const uint8_t glyphs[][5] = {
    {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36}, {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41}, {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f}, {0x00, 0x41, 0x7f, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41}, {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f}, {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e}, {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01}, {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f}, {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0x00, 0x42, 0x7f, 0x40, 0x00}, {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1e}};

// Copy Demo text implementation
static void text_at(const char *message, int x, int y, int scale, uint16_t color)
{
    int length = strlen(message);
    if (x < 0)
        x = 0;
    uint16_t *fb = framebuffers[back_buffer];
    for (int i = 0; i < length; ++i)
    {
        char c = message[i];
        int glyph = c >= 'A' && c <= 'Z' ? c - 'A' : c >= '0' && c <= '9' ? c - '0' + 26
                                                                          : -1;
        static const uint8_t percent_glyph[5] = {0x63, 0x13, 0x08, 0x64, 0x63};
        static const uint8_t unknown_glyph[5] = {0x02, 0x01, 0x51, 0x09, 0x06};
        const uint8_t *shape = glyph >= 0 ? glyphs[glyph] : c == '%' ? percent_glyph
                                                        : c == '?'   ? unknown_glyph
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
                            if (xx >= 0 && yy >= 0 && xx < SCREEN_WIDTH && yy < SCREEN_HEIGHT)
                                fb[yy * SCREEN_WIDTH + xx] = color;
                        }
    }
}

static void text(const char *message, int y, int scale, uint16_t color)
{
    text_at(message, (SCREEN_WIDTH - strlen(message) * 6 * scale) / 2, y, scale, color);
}

void setBatteryGliph(int percent)
{
    if (percent == battery_percent)
        return;
    battery_percent = percent;
    if (!frame_ready)
        return;
    wait_for_back_buffer();
    memcpy(framebuffers[back_buffer], framebuffers[back_buffer ^ 1], PANEL_BYTES);
    present_back_buffer(false);
}

static void draw_information_overlay(void)
{
    status_footer_t status = {.enabled = information_enabled, .fps_x100 = footer_fps_x100, .battery_percent = battery_percent, .max_fps = maximum_fps_enabled};
    status_footer_draw(framebuffers[back_buffer], SCREEN_WIDTH, SCREEN_HEIGHT, &status);
}

void board_setStatus(const char *a, const char *b, const char *c)
{
    wait_for_back_buffer();
    memset(framebuffers[back_buffer], 0, PANEL_BYTES);
    text(a, 300, 3, 0x07ff);
    text(b, 370, 2, 0xffff);
    text(c, 410, 2, 0xffff);
    present_back_buffer(true);
}

void buttonInit(void)
{
    gpio_config_t config = {.pin_bit_mask = 1ULL, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
    ESP_ERROR_CHECK(gpio_config(&config));
}

bool ifButtonDown(void) { return gpio_get_level(0) == 0; }
