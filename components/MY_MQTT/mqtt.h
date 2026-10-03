#ifndef MQTT_H
#define MQTT_H

#include "esp_err.h"
#include <stdbool.h>

/* 设备与 Wi-Fi 凭据存放在未入库的 app_secrets.h
 * （首次 clone 后请把 app_secrets.h.example 复制为 app_secrets.h 并填入自己的值）*/
#include "app_secrets.h"

/**
 * @brief 属性设置回调 — 平台下发属性修改请求
 * @param msg_id      消息ID，回复时需原样带回
 * @param params_json 属性参数原始JSON
 * @param data_len    payload长度
 */
typedef void (*onenet_property_set_cb_t)(const char *msg_id,
                                          const char *params_json,
                                          int data_len);

/**
 * @brief 属性查询回调 — 平台查询设备当前属性
 * @param msg_id 消息ID，回复时需原样带回
 */
typedef void (*onenet_property_query_cb_t)(const char *msg_id);

/**
 * @brief 服务调用回调 — 平台调用设备服务
 * @param msg_id      消息ID，回复时需原样带回
 * @param service_id  服务标识（从Topic中提取）
 * @param params_json 服务参数原始JSON（即收到的完整payload）
 * @param data_len    payload长度
 */
typedef void (*onenet_service_cb_t)(const char *msg_id,
                                     const char *service_id,
                                     const char *params_json,
                                     int data_len);


esp_err_t onenet_start(void);
bool      onenet_is_connected(void);

esp_err_t onenet_report_properties(const char *params_json);

esp_err_t onenet_report_property(const char *name, float value);
esp_err_t onenet_report_property_int(const char *name, int value);

esp_err_t onenet_report_event(const char *event_id, const char *params_json);

void onenet_register_property_set_callback(onenet_property_set_cb_t cb);
void onenet_register_property_query_callback(onenet_property_query_cb_t cb);
void onenet_register_service_callback(onenet_service_cb_t cb);


esp_err_t onenet_property_set_reply(const char *msg_id, int code, const char *msg);
esp_err_t onenet_property_query_reply(const char *msg_id, const char *params_json);
esp_err_t onenet_service_reply(const char *service_id, const char *msg_id,
                               int code, const char *msg);

#endif
