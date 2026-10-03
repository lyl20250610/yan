idf_component_register(
    SRCS "mqtt.c" "onenet_token.c"
    INCLUDE_DIRS "."
    REQUIRES mbedtls mqtt esp_timer esp_event
)