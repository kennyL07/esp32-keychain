idf_component_register(SRCS "main.c" "board.c" "player.c" "usb_disk.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_sdmmc fatfs sdmmc esp_timer)
