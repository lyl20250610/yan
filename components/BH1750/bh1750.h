#ifndef BH1750_H
#define BH1750_H

#include "driver/i2c.h"


esp_err_t bh1750_init(i2c_port_t i2c_num, uint8_t addr);
float bh1750_get_lux(void);

esp_err_t bh1750_update(void);
esp_err_t bh1750_reinit(void);

#endif