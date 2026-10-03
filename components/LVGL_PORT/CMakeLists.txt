idf_component_register(
    SRCS "lvgl_port.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd driver DISPLAY TOUCH  # 添加TOUCH依赖
)