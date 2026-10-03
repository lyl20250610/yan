#ifndef UI_H
#define UI_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// UI初始化函数
void ui_init(void);

// 更新传感器数据显示
void ui_update_sensors(float temp, float humi, float light, uint16_t tvoc, uint16_t co2);

// 更新WiFi状态显示
void ui_update_wifi_status(bool connected);

// 更新MQTT状态显示
void ui_update_mqtt_status(bool connected);

// 更新小智AI状态显示
void ui_update_ai_status(const char *status);

// 更新AI语音回复显示（线程安全）
void ui_update_ai_reply(const char *text);

// 聊天Tab相关（面向小智AI的文字交互）
typedef void (*ui_chat_send_cb_t)(const char *text);
void ui_chat_set_send_callback(ui_chat_send_cb_t cb);
void ui_chat_add_message(const char *text, bool is_user);
void ui_chat_set_enabled(bool enabled);

// 页面切换函数
void ui_switch_to_next_page(void);
void ui_switch_to_prev_page(void);

#ifdef __cplusplus
}
#endif

#endif
