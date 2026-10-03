idf_component_register(
    SRCS "main.c" "ui.c"
    REQUIRES
        APDS9960
        BH1750
        DHT11
        DISPLAY
        LVGL_PORT
        MY_MQTT
        SGP30
        TOUCH
        WIFI
        VOICE
        XIAOZHI_AI
        FONT
        BLE
        json
)

component_compile_options(-ffast-math -O3 -Wno-error=format -Wno-format)
