#ifndef BLE_H
#define BLE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** BLE 设备控制回调: cmd 如 "screen:0"（屏幕背光开关） */
typedef void (*ble_ctrl_cb_t)(const char *cmd);

/**
 * @brief 初始化 BLE 外设 (NimBLE GATT Server)
 *
 * 启动后会以 "AIoT-SmartHome" 为名广播，
 * 手机可通过 nRF Connect / LightBlue 连接查看传感器数据。
 */
void ble_init(void);

// ---- 更新传感器数据（调用后自动 Notify 已订阅的客户端）----
void ble_update_temperature(float temp);
void ble_update_humidity(float humi);
void ble_update_light(float lux);
void ble_update_tvoc(uint16_t tvoc);
void ble_update_co2(uint16_t co2);

/** 更新 Wi-Fi 连接状态 */
void ble_set_wifi_status(bool connected);

/** 注册设备控制回调（手机下发指令时触发）*/
void ble_set_control_callback(ble_ctrl_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif
