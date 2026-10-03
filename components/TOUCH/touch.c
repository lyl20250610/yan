#include "touch.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#define I2C_TOUCH_PORT   I2C_NUM_1
#define FT6336_ADDR      0x38

static const char *TAG = "TOUCH";

esp_err_t touch_init(const touch_pin_cfg_t *pin_cfg)
{
    if (!pin_cfg) {
        ESP_LOGE(TAG, "pin_cfg is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "Touch init (SDA=%d SCL=%d INT=%d RST=%d)",
             pin_cfg->sda_pin, pin_cfg->scl_pin,
             pin_cfg->int_pin, pin_cfg->rst_pin);

    // 硬件复位 CTP-RST（如果引脚有效）
    if (pin_cfg->rst_pin >= 0) {
        gpio_config_t rst_conf = {
            .pin_bit_mask = (1ULL << pin_cfg->rst_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&rst_conf);
        gpio_set_level(pin_cfg->rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(pin_cfg->rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        ESP_LOGI(TAG, "Touch reset done (GPIO%d)", pin_cfg->rst_pin);
    }

    // 配置中断引脚（如果有效）
    if (pin_cfg->int_pin >= 0) {
        gpio_config_t int_conf = {
            .pin_bit_mask = (1ULL << pin_cfg->int_pin),
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&int_conf);
    }

    // 初始化 I2C
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = pin_cfg->sda_pin,
        .scl_io_num = pin_cfg->scl_pin,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(I2C_TOUCH_PORT, &conf);
    if (err != ESP_OK) {
        return err;
    }

    err = i2c_driver_install(I2C_TOUCH_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Touch I2C init OK");
    return ESP_OK;
}

bool touch_read(touch_point_t *tp)
{
    uint8_t reg = 0x02; 
    uint8_t data[6];
    
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (FT6336_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 6, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(I2C_TOUCH_PORT, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        tp->pressed = false;
        return false;
    }
    
    if (data[0] == 1) {
        tp->pressed = true;
        int raw_x = ((data[1] & 0x0F) << 8) | data[2];
        int raw_y = ((data[3] & 0x0F) << 8) | data[4];

        // 调试：记录原始值范围（正式版删除此日志）
        static int rx_min = 9999, rx_max = 0, ry_min = 9999, ry_max = 0;
        if (raw_x < rx_min) rx_min = raw_x;
        if (raw_x > rx_max) rx_max = raw_x;
        if (raw_y < ry_min) ry_min = raw_y;
        if (raw_y > ry_max) ry_max = raw_y;
        static int log_cnt = 0;
        if (++log_cnt % 50 == 0) {
            ESP_LOGI("TOUCH", "raw range: x=[%d..%d] y=[%d..%d]", rx_min, rx_max, ry_min, ry_max);
        }

        // swap XY + 双轴等比缩放 + Y镜像
        tp->x = (raw_y * 4) / 3;          // 0..240 → 0..320
        tp->y = 240 - (raw_x * 3) / 4;    // 0..320 → 240..0

        if (tp->x > 320) tp->x = 320;
        if (tp->y > 240) tp->y = 240;
    } else {
        tp->pressed = false;
    }
    return true;
}