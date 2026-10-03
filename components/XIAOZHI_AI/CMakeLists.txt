idf_component_register(
    SRCS "xiaozi_ai.c"
    INCLUDE_DIRS "."
    REQUIRES
        driver
        freertos
        espressif__esp_websocket_client
        esp_http_client
        mqtt
        78__esp-opus
        espressif__esp-sr
        nvs_flash
        VOICE
        WIFI
)
