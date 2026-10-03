#ifndef APDS9960_H
#define APDS9960_H

#include "esp_err.h"
#include "driver/i2c.h"


#define GESTURE_UP      1   // 上滑
#define GESTURE_DOWN    2   // 下滑
#define GESTURE_LEFT    3   // 左滑
#define GESTURE_RIGHT   4   // 右滑
#define GESTURE_NEAR    5   
#define GESTURE_FAR     6   

esp_err_t apds9960_init(i2c_port_t i2c_num);
esp_err_t apds9960_read_gesture(uint8_t *gesture);

#endif