#include "display.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "DISPLAY";

#define SPI_HOST       SPI2_HOST

static esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t s_io_handle = NULL;
static int s_bl_pin = -1;
static SemaphoreHandle_t s_flush_sem = NULL;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    SemaphoreHandle_t sem = (SemaphoreHandle_t)user_ctx;
    BaseType_t higher_prio_woken = pdFALSE;
    xSemaphoreGiveFromISR(sem, &higher_prio_woken);
    return higher_prio_woken == pdTRUE;
}

esp_err_t display_init(const display_pin_cfg_t *pin_cfg)
{
    if (!pin_cfg) {
        ESP_LOGE(TAG, "pin_cfg is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    s_bl_pin = pin_cfg->bl_pin;

    s_flush_sem = xSemaphoreCreateBinary();
    if (!s_flush_sem) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "Initialize LCD panel (CS=%d DC=%d RST=%d BL=%d)",
             pin_cfg->cs_pin, pin_cfg->dc_pin, pin_cfg->rst_pin, pin_cfg->bl_pin);

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = pin_cfg->dc_pin,
        .cs_gpio_num = pin_cfg->cs_pin,
        .pclk_hz = 27 * 1000 * 1000,  // restored — color issue fixed by LV_COLOR_16_SWAP + 0xF6
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,        // increased from 4 for smoother scrolling
        .on_color_trans_done = on_color_trans_done,
        .user_ctx = s_flush_sem,
    };
    esp_err_t ret = esp_lcd_new_panel_io_spi(SPI_HOST, &io_config, &s_io_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel IO: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = pin_cfg->rst_pin,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,  // LV_COLOR_16_SWAP already outputs big-endian
        .bits_per_pixel = 16,
    };
    ret = esp_lcd_new_panel_ili9341(s_io_handle, &panel_config, &panel_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create panel: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_lcd_panel_reset(panel_handle);
    esp_lcd_panel_init(panel_handle);
    esp_lcd_panel_invert_color(panel_handle, false);
    esp_lcd_panel_mirror(panel_handle, false, false);
    esp_lcd_panel_disp_on_off(panel_handle, true);

    gpio_config_t bl_config = {
        .pin_bit_mask = (1ULL << pin_cfg->bl_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&bl_config);
    gpio_set_level(pin_cfg->bl_pin, 1);

    ESP_LOGI(TAG, "Display init OK");
    return ESP_OK;
}

esp_lcd_panel_handle_t display_get_panel(void)
{
    return panel_handle;
}

esp_lcd_panel_io_handle_t display_get_io(void)
{
    return s_io_handle;
}

void display_backlight_set(uint8_t brightness)
{
    if (s_bl_pin >= 0) {
        gpio_set_level(s_bl_pin, brightness > 0);
    }
}

esp_err_t display_wait_for_flush(uint32_t timeout_ms)
{
    if (!s_flush_sem) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_flush_sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "Flush wait timeout (%lu ms)", (unsigned long)timeout_ms);
    return ESP_ERR_TIMEOUT;
}
