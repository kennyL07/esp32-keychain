idf_component_register(SRCS "storage_api.c" "usb_api.c" "usb_control.c" "main.c" "board.c" "status_footer.c" "battery.c" "player.c" "media.c" "usb_disk.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_sdmmc
    fatfs sdmmc esp_timer nvs_flash esp_adc)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=msc_storage_mount_to_usb")
