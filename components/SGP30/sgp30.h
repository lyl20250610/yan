#ifndef SGP30_H
#define SGP30_H

#include "esp_err.h"
#include "driver/i2c.h"

typedef struct sgp30_t *sgp30_handle_t;

sgp30_handle_t sgp30_create(i2c_port_t i2c_num);
void sgp30_delete(sgp30_handle_t handle);
esp_err_t sgp30_init(sgp30_handle_t handle);
esp_err_t sgp30_read_air_quality(sgp30_handle_t handle, uint16_t *tvoc, uint16_t *co2);

esp_err_t sgp30_update(sgp30_handle_t handle);
uint16_t sgp30_get_tvoc(void);
uint16_t sgp30_get_co2(void);

// 获取当前传感器的基线值
esp_err_t sgp30_get_baseline(sgp30_handle_t handle, uint16_t *co2_baseline, uint16_t *tvoc_baseline);

// 恢复传感器的基线值
esp_err_t sgp30_restore_baseline(sgp30_handle_t handle, uint16_t co2_baseline, uint16_t tvoc_baseline);
#endif
