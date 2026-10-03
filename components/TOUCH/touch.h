#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    int x;
    int y;
    bool pressed;
} touch_point_t;

/**
 * @brief 触摸模块引脚配置
 */
typedef struct {
    int sda_pin;    ///< I2C SDA
    int scl_pin;    ///< I2C SCL
    int int_pin;    ///< 中断引脚（< 0 则不使用中断，采用轮询）
    int rst_pin;    ///< 复位引脚（< 0 则不使用硬件复位）
} touch_pin_cfg_t;

/**
 * @brief 初始化触摸模块
 * @param pin_cfg 引脚配置
 * @return ESP_OK 成功
 */
esp_err_t touch_init(const touch_pin_cfg_t *pin_cfg);
bool touch_read(touch_point_t *tp);

#endif