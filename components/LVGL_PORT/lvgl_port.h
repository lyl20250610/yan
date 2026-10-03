#ifndef LVGL_PORT_H
#define LVGL_PORT_H

#include "esp_err.h"

esp_err_t lvgl_port_init(void);
void lvgl_port_task(void *arg);

#endif
