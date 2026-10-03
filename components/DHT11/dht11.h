#ifndef DHT11_H
#define DHT11_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

esp_err_t dht11_init(gpio_num_t pin);
esp_err_t dht11_read(float *temperature, float *humidity);


float dht11_get_temperature(void);
float dht11_get_humidity(void);
bool dht11_is_valid(void);
void dht11_start_task(void);

#endif
