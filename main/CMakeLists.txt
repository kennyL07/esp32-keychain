idf_component_register(SRCS "main.c" "board.c" "player.c" "usb_disk.c" "status_footer.c" "battery.c" "media.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_driver_sdmmc fatfs sdmmc esp_timer nvs_flash esp_adc)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=msc_storage_mount_to_usb")
