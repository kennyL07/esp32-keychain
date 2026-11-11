idf_component_register(SRCS "main.c" "board.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc)
