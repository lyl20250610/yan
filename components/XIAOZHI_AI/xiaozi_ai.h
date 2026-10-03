#ifndef XIAOZHI_AI_H
#define XIAOZHI_AI_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 小智AI对话模式
 */
typedef enum {
    XIAOZHI_MODE_MANUAL,  /**< 手动按键对话 */
    XIAOZHI_MODE_AUTO,    /**< 自动 VAD 对话（唤醒词触发）*/
} xiaozhi_mode_t;

/**
 * @brief 小智AI事件类型
 */
typedef enum {
    XIAOZHI_EVENT_CONNECTED,      
    XIAOZHI_EVENT_DISCONNECTED,   
    XIAOZHI_EVENT_ASR_TEXT,       
    XIAOZHI_EVENT_TTS_START,     
    XIAOZHI_EVENT_TTS_STOP,       
    XIAOZHI_EVENT_LISTENING,      
    XIAOZHI_EVENT_THINKING,       
    XIAOZHI_EVENT_SPEAKING,    
    XIAOZHI_EVENT_LLM_TEXT,      
    XIAOZHI_EVENT_ERROR,         
} xiaozhi_event_t;

/**
 * @brief //事件回调函数类型
 * @param event  //事件类型
 * @param data   //携带的数据
 * @param user_data  //用户自定义数据
 */
typedef void (*xiaozhi_event_cb_t)(xiaozhi_event_t event, const char *data, void *user_data);

/**
 * @brief 小智AI配置
 */
typedef struct {
    const char *server_url;       /**< 服务器 WebSocket URL，如 "wss://api.tenclass.net/xiaozhi/v1/" */
    const char *access_token;     /**< 认证 Token，NULL 则匿名 */
    xiaozhi_mode_t mode;          /**< 对话模式 */
    xiaozhi_event_cb_t event_cb;  /**< 事件回调 */
    void *user_data;              /**< 传给回调的用户数据 */
} xiaozhi_ai_config_t;

/**
 * @brief 初始化并连接小智AI
 *
 * 处理：WiFi 检查 → WebSocket 连接 → hello 握手 →
 *        Opus 编解码器初始化 → 音频采集/播放任务启动
 *
 * @param cfg 配置参数
 * @return ESP_OK 成功
 */
esp_err_t xiaozhi_ai_init(const xiaozhi_ai_config_t *cfg);

/**
 * @brief 小智 AI MCP 握手是否已完成
 * @return true = 握手完成，可以安全并发网络操作；false = 正在连接中
 */
bool xiaozhi_ai_is_ready(void);

/**
 * @brief 启动聆听（手动模式）
 */
esp_err_t xiaozhi_ai_start_listen(void);

/**
 * @brief 停止聆听（手动模式）
 */
esp_err_t xiaozhi_ai_stop_listen(void);

/**
 * @brief 通知检测到唤醒词（自动模式）
 */
esp_err_t xiaozhi_ai_wake_word_detected(const char *wake_text);

/**
 * @brief 中断当前对话
 */
esp_err_t xiaozhi_ai_abort(void);

/**
 * @brief 发送文字消息到小智AI
 * @param text UTF-8 文字
 * @return ESP_OK 成功
 */
esp_err_t xiaozhi_ai_send_text(const char *text);

/**
 * @brief 获取设备激活码（6位数字）
 * @return 激活码字符串，未获取到返回 NULL
 */
const char *xiaozhi_ai_get_activation_code(void);

/**
 * @brief 
 * @param mute 
 */
void xiaozhi_ai_mute_tts(bool mute);

/**
 * @brief 反初始化
 */
void xiaozhi_ai_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
