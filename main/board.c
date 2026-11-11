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

static esp_lcd_panel_handle_t panel;
static uint16_t *framebuffer;

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

void boardBrightness(unsigned value)
{
    if (value > 255)
        value = 255;

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 255 - value);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

esp_err_t boardDisplayInit(void)
{
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
    rgb.num_fbs = 1;
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
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(panel, 1, (void **)&framebuffer));
    memset(framebuffer, 0, panelBytes);
    return ESP_OK;
}

uint16_t *boardBackBuffer(void) { return framebuffer; }

void boardPresent(const uint16_t *pixels)
{
    if (pixels != framebuffer) memcpy(framebuffer, pixels, panelBytes);
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, panelWidth, panelHeight, framebuffer));
}
