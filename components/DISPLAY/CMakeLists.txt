idf_component_register(
    SRCS "display.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_lcd_ili9341
)