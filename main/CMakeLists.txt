idf_component_register(SRCS "main.c" "board.c" "status_footer.c" "battery.c" "player.c" "storage_api.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_sdmmc
    fatfs sdmmc esp_timer esp_netif esp_http_server nvs_flash esp_adc
    esp_driver_usb_serial_jtag esp_http_client)
