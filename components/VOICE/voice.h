#ifndef VOICE_H
#define VOICE_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 语音模块引脚配置
 */
typedef struct {
    int bclk_pin;   ///< I2S Bit Clock
    int ws_pin;     ///< I2S Word Select (LRC)
    int din_pin;    ///< I2S Data In (麦克风)
    int dout_pin;   ///< I2S Data Out (MAX98357 功放)
    int spk_sd_pin; ///< MAX98357 SD_MODE 引脚（-1 = 未连接/功放常开，>=0 = 静音控制）
} voice_pin_cfg_t;

/**
 * @brief 初始化语音 I/O
 * @details 必须在 voice_module_init() 之前调用。
 *          提供 voice_read_audio() / voice_play_audio() 所需的硬件，
 *          但不加载 AFE 模型、不启动 voice_task。
 * @param pin_cfg 引脚配置
 * @return
 *     - ESP_OK: 成功
 *     - ESP_FAIL: 失败
 */
esp_err_t voice_io_init(const voice_pin_cfg_t *pin_cfg);

/**
 * @brief 初始化语音模块（唤醒词+命令词识别）
 * @details 加载 AFE 模型并启动语音处理任务。
 *          必须在 voice_io_init() 之后调用。
 * @return
 *     - ESP_OK: 成功
 *     - ESP_FAIL: 失败
 */
esp_err_t voice_module_init(void);

/**
 * @brief 通过 MAX98357 播放 PCM 音频数据
 * @param data PCM 数据指针
 * @param len 数据长度
 * @return
 *     - ESP_OK: 成功
 *     - ESP_ERR_INVALID_STATE: TX 通道未初始化
 */
esp_err_t voice_play_audio(const int16_t *data, size_t len);

/**
 * @brief 播放简单提示音（方波蜂鸣）
 * @param freq_hz 频率（Hz），建议 500~4000
 * @param duration_ms 持续时间（ms）
 * @return
 *     - ESP_OK: 成功
 *    - ESP_ERR_INVALID_STATE: TX 通道未初始化
 */
esp_err_t voice_play_tone(uint32_t freq_hz, uint32_t duration_ms);

/**
 * @brief 获取 I2S 麦克风通道句柄
 * @return I2S RX 通道句柄
 */
void *voice_get_rx_channel(void);

/**
 * @brief 从共享环形缓冲区读取麦克风音频数据（供外部模块如小智AI使用）
 * @param buf     输出缓冲区
 * @param max_samples  最多读取的采样点数
 * @param timeout 超时时间（FreeRTOS ticks），0 表示不阻塞
 * @return 实际读取的采样点数，-1 表示环形缓冲区未分配
 */
int voice_read_audio(int16_t *buf, int max_samples, TickType_t timeout);

/** 唤醒词回调类型 */
typedef void (*voice_wake_cb_t)(void);

/**
 * @brief 注册唤醒词回调（检测到唤醒词时调用）
 * @param cb 回调函数指针
 */
void voice_set_wake_callback(voice_wake_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif