#ifndef DISPLAY_H
#define DISPLAY_H

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"

#define DISPLAY_WIDTH   320
#define DISPLAY_HEIGHT  240

typedef struct {
    int cs_pin;
    int dc_pin;
    int rst_pin;
    int bl_pin;
} display_pin_cfg_t;

esp_err_t display_init(const display_pin_cfg_t *pin_cfg);
esp_lcd_panel_handle_t display_get_panel(void);
void display_backlight_set(uint8_t brightness);
esp_err_t display_wait_for_flush(uint32_t timeout_ms);

#endif
