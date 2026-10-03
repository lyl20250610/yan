idf_component_register(
    SRCS "voice.c"
    INCLUDE_DIRS "."
    REQUIRES 
        driver 
        freertos 
        espressif__esp-sr 
        esp_timer
)