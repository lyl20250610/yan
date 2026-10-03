// ...existing code...
#ifndef WIFI_H
#define WIFI_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __cplusplus
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L
#include <stdbool.h>
#else
typedef unsigned char bool;
#define true 1
#define false 0
#endif
#endif

esp_err_t wifi_init_sta(const char *ssid, const char *password);
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif 