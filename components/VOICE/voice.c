#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_process_sdkconfig.h"
#include "esp_partition.h"
#include "model_path.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "voice.h"

static const char *TAG = "VOICE_MODULE";

// 音频参数定义
#define I2S_PORT            I2S_NUM_0
#define I2S_SAMPLE_RATE     16000
#define I2S_BITS_SAMPLE     16

// ---- 共享音频环形缓冲区（voice_task 写入 → capture_task/xiaozi_ai 读取）----
#define VOICE_RING_SAMPLES  (16000 * 2)   // 2 秒 16kHz 单声道，存 PSRAM
static int16_t *g_audio_ring = NULL;
static volatile int g_ring_write = 0;
static volatile int g_ring_read  = 0;
static portMUX_TYPE g_ring_lock = portMUX_INITIALIZER_UNLOCKED;

// 音频前端 (AFE) 句柄 
static const esp_afe_sr_iface_t *afe_handle = NULL;
static esp_afe_sr_data_t         *afe_data = NULL;

// 命令识别 (MultiNet) 句柄 
static const esp_mn_iface_t *multinet_handle = NULL;

// 定义一个队列，用于向主任务发送识别出的命令ID
QueueHandle_t voice_command_queue = NULL;

// 唤醒词回调（供外部模块如小智AI使用）
static voice_wake_cb_t g_wake_cb = NULL;

// I2S 通道句柄
static i2s_chan_handle_t voice_rx_chan = NULL;
static i2s_chan_handle_t voice_tx_chan = NULL;

// MAX98357 SD_MODE 引脚（可选，用于静音/关断控制）
// 未配置时(<0)，功放常开；配置后，仅播放时解除静音
static int g_spk_sd_pin = -1;
static bool g_spk_muted = true;

static esp_err_t voice_i2s_init(const voice_pin_cfg_t *pin_cfg) {
    i2s_std_config_t std_config = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BITS_SAMPLE, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {0},
    };

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &voice_tx_chan, &voice_rx_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2S channels");
        return ret;
    }
    // RX 通道（麦克风）
    std_config.gpio_cfg = (i2s_std_gpio_config_t){
        .mclk = I2S_GPIO_UNUSED,
        .bclk = pin_cfg->bclk_pin,
        .ws   = pin_cfg->ws_pin,
        .dout = I2S_GPIO_UNUSED,
        .din  = pin_cfg->din_pin,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    };
    ret = i2s_channel_init_std_mode(voice_rx_chan, &std_config);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(voice_rx_chan);
    if (ret != ESP_OK) return ret;

    // TX 通道（MAX98357 功放）
    std_config.gpio_cfg = (i2s_std_gpio_config_t){
        .mclk = I2S_GPIO_UNUSED,
        .bclk = I2S_GPIO_UNUSED,
        .ws   = I2S_GPIO_UNUSED,
        .dout = pin_cfg->dout_pin,
        .din  = I2S_GPIO_UNUSED,
        .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    };
    ret = i2s_channel_init_std_mode(voice_tx_chan, &std_config);
    if (ret != ESP_OK) return ret;
    ret = i2s_channel_enable(voice_tx_chan);
    if (ret != ESP_OK) return ret;

    // MAX98357: 预填充 TX DMA 缓冲区为静默值，避免 enable 后输出随机 DMA 残留数据
    // ESP32-S3 I2S TX DMA buffer 默认 4KB，写满 4096 字节的零采样
    {
        int16_t *silence = calloc(1, 4096);
        if (silence) {
            size_t written = 0;
            // 连续写 3 次确保 DMA 描述符链全部填充
            for (int i = 0; i < 3; i++) {
                i2s_channel_write(voice_tx_chan, silence, 4096, &written, pdMS_TO_TICKS(100));
            }
            free(silence);
            ESP_LOGI(TAG, "TX DMA silence pre-fill OK");
        }
    }

    // 初始化 SD_MODE 引脚（默认拉低 → 静音），未配置则功放常开
    g_spk_sd_pin = pin_cfg->spk_sd_pin;
    if (g_spk_sd_pin >= 0) {
        gpio_config_t sd_cfg = {
            .pin_bit_mask = (1ULL << g_spk_sd_pin),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,  // 默认拉低 → 静音
        };
        gpio_config(&sd_cfg);
        gpio_set_level(g_spk_sd_pin, 0);
        g_spk_muted = true;
        ESP_LOGI(TAG, "Speaker SD_MODE GPIO%d → muted", g_spk_sd_pin);
    } else {
        g_spk_muted = false;
    }
    return ret;
}

static void voice_task(void *arg) {
    int audio_chunk_size = afe_handle->get_feed_chunksize(afe_data);
    int feed_channel_num = afe_handle->get_feed_channel_num(afe_data);
    int feed_bytes = audio_chunk_size * sizeof(int16_t) * feed_channel_num;
    bool has_multinet = (multinet_handle != NULL);

    ESP_LOGI(TAG, "voice_task start: chunk=%d ch=%d bytes=%d, internal free=%u",
             audio_chunk_size, feed_channel_num, feed_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    int16_t *i2s_buffer = (int16_t *)heap_caps_malloc(feed_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    model_iface_data_t *mn_data = NULL;

    if (!i2s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate audio buffer (%d bytes, internal free=%u, largest=%u)",
                 feed_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "voice_task i2s_buffer OK: %d bytes", feed_bytes);

    if (has_multinet) {
        mn_data = multinet_handle->create(MULTINET_MODEL_NAME, 3000);
        if (!mn_data) {
            ESP_LOGW(TAG, "MultiNet data create failed — command recognition disabled");
            has_multinet = false;
        } else {
        
            esp_mn_commands_update_from_sdkconfig(multinet_handle, mn_data);
            ESP_LOGI(TAG, "MultiNet command recognition enabled");
        }
    }

    int loop_cnt = 0;
    int no_data_cnt = 0;
    while (1) {
        size_t bytes_read = 0;
        esp_err_t read_ret = i2s_channel_read(voice_rx_chan, i2s_buffer, feed_bytes, &bytes_read, pdMS_TO_TICKS(1000));
        if (read_ret != ESP_OK || bytes_read == 0) {
            if (++no_data_cnt % 10 == 1) {
                ESP_LOGW(TAG, "I2S read: ret=%s, bytes=%d, cnt=%d",
                         esp_err_to_name(read_ret), (int)bytes_read, no_data_cnt);
            }
            continue;
        }
        no_data_cnt = 0;

        // 写入共享环形缓冲区，供外部模块（如小智AI）使用 —— 解决 I2S 双读冲突
        if (g_audio_ring && bytes_read > 0) {
            int samples = bytes_read / sizeof(int16_t);
            int16_t *raw = i2s_buffer;
            int w = g_ring_write;
            for (int i = 0; i < samples; i++) {
                g_audio_ring[w] = raw[i];
                w = (w + 1) % VOICE_RING_SAMPLES;
            }
            portENTER_CRITICAL(&g_ring_lock);
            g_ring_write = w;
            portEXIT_CRITICAL(&g_ring_lock);
        }

        afe_handle->feed(afe_data, i2s_buffer);
        afe_fetch_result_t *res = afe_handle->fetch(afe_data);

        // 每 500 轮（~5秒）打印一次心跳
        if (++loop_cnt % 500 == 0) {
            ESP_LOGI(TAG, "voice_task alive, loop=%d, bytes=%d, internal_free=%u",
                     loop_cnt, (int)bytes_read,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        }

        if (res && res->ret_value == ESP_OK) {
            if (res->wakeup_state == WAKENET_DETECTED) {
                ESP_LOGI(TAG, "*** Wake word detected! callback=%s ***",
                         g_wake_cb ? "registered" : "NULL");
                if (g_wake_cb) g_wake_cb();
                if (has_multinet) {
                    multinet_handle->clean(mn_data);
                    ESP_LOGI(TAG, "Start listening for commands...");
                }
            }
            if (has_multinet) {
                esp_mn_state_t mn_state = multinet_handle->detect(mn_data, res->data);
                if (mn_state == ESP_MN_STATE_DETECTED) {
                    esp_mn_results_t *mn_result = multinet_handle->get_results(mn_data);
                    if (mn_result->num > 0) {
                        uint16_t cmd_id = mn_result->command_id[0];
                        ESP_LOGI(TAG, "Command detected, ID: %d", cmd_id);
                        xQueueSend(voice_command_queue, &cmd_id, pdMS_TO_TICKS(100));
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(i2s_buffer);
    if (mn_data) multinet_handle->destroy(mn_data);
    vTaskDelete(NULL);
}

esp_err_t voice_io_init(const voice_pin_cfg_t *pin_cfg) {
    if (!pin_cfg) {
        ESP_LOGE(TAG, "pin_cfg is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    // 1. 创建命令队列
    voice_command_queue = xQueueCreate(10, sizeof(uint16_t));
    if (voice_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create voice command queue");
        return ESP_FAIL;
    }

    // 2. 初始化 I2S 麦克风
    if (voice_i2s_init(pin_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "I2S init failed");
        return ESP_FAIL;
    }

    // 3. 分配音频共享环形缓冲区（PSRAM，供小智 AI 读取音频）
    if (!g_audio_ring) {
        g_audio_ring = (int16_t *)heap_caps_malloc(
            VOICE_RING_SAMPLES * sizeof(int16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g_audio_ring) {
            ESP_LOGW(TAG, "Ring buffer alloc failed — xiaozi_ai will not get audio");
        } else {
            ESP_LOGI(TAG, "Audio ring buffer OK: %d samples (%d bytes)",
                     VOICE_RING_SAMPLES, (int)(VOICE_RING_SAMPLES * sizeof(int16_t)));
        }
    }

    ESP_LOGI(TAG, "Voice I/O ready (I2S + ring buffer)");
    return ESP_OK;
}

esp_err_t voice_module_init(void) {
    // 3. 加载模型
    srmodel_list_t *models = NULL;
    const esp_partition_t *part = esp_partition_find_first(0x01, 0x82, "model");
    if (part) {
        ESP_LOGI(TAG, "Model partition size: %lu KB", (uint32_t)(part->size / 1024));
        uint8_t buf[4];
        if (esp_partition_read(part, 0, buf, 4) == ESP_OK) {
            uint32_t model_count = buf[0] | ((uint32_t)buf[1] << 8)
                                 | ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
            if (model_count > 0 && model_count < 50) {
                models = esp_srmodel_init("model");
            } else {
                ESP_LOGW(TAG, "Model partition data invalid (count=%lu), skipping", model_count);
            }
        } else {
            ESP_LOGW(TAG, "Cannot read model partition, skipping");
        }
    }
    if (!models) {
        ESP_LOGE(TAG, "Voice models unavailable (model 分区缺失或未烧录 srmodels.bin): "
                      "wake word + mic uplink disabled");
        return ESP_FAIL;
    }

    // 4. 初始化音频前端 (AFE)
    afe_config_t *afe_config = afe_config_init("M", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    if (!afe_config) {
        ESP_LOGE(TAG, "AFE config init failed");
        return ESP_FAIL;
    }
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    afe_handle = esp_afe_handle_from_config(afe_config);
    afe_data = afe_handle->create_from_config(afe_config);
    if (!afe_data) {
        ESP_LOGE(TAG, "AFE create failed");
        return ESP_FAIL;
    }

    // 5. 初始化命令识别 (MultiNet) 
    multinet_handle = esp_mn_handle_from_name(MULTINET_MODEL_NAME);
    if (!multinet_handle) {
        ESP_LOGW(TAG, "MultiNet handle not found — command recognition disabled, wake word only");
    }

    // 6. 创建语音处理任务（栈分配到 PSRAM，释放内部 RAM 给 I2S DMA 和 AFE）
    //    优先级 2（低于 lwIP 线程），避免抢占 Core 1 上的网络处理
    TaskHandle_t task_handle = NULL;
    BaseType_t ret = xTaskCreateWithCaps(voice_task, "voice_task", 16384, NULL, 2,
                                         &task_handle, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "voice_task create FAILED (16KB stack, internal free=%u, largest=%u)",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Voice module initialized successfully (wake word %s, command recognition %s)",
             "enabled", multinet_handle ? "enabled" : "disabled");
    return ESP_OK;
}

void voice_set_wake_callback(voice_wake_cb_t cb)
{
    g_wake_cb = cb;
}

//音频播放功能（MAX98357 功放

// 扬声器静音/解除（仅当 SD_MODE 引脚已配置时生效）
static void voice_speaker_mute(void)
{
    if (g_spk_sd_pin >= 0 && !g_spk_muted) {
        gpio_set_level(g_spk_sd_pin, 0);
        g_spk_muted = true;
    }
}

static void voice_speaker_unmute(void)
{
    if (g_spk_sd_pin >= 0 && g_spk_muted) {
        // 先写一小段静默音频让 I2S 时钟稳定，再解除功放静音
        int16_t silence[64] = {0};
        size_t written = 0;
        i2s_channel_write(voice_tx_chan, silence, sizeof(silence), &written, pdMS_TO_TICKS(50));
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(g_spk_sd_pin, 1);
        g_spk_muted = false;
    }
}

// 软斜坡参数: 5ms 淡入/淡出（16000Hz * 0.005s = 80 采样）
#define RAMP_SAMPLES  80

esp_err_t voice_play_audio(const int16_t *data, size_t len) {
    if (!voice_tx_chan || !data || len == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    voice_speaker_unmute();

    int total_samples = len / sizeof(int16_t);
    int16_t *ramped = NULL;

    // 数据足够长才做软斜坡，否则直接发送
    if (total_samples > RAMP_SAMPLES * 2) {
        ramped = malloc(len);
        if (ramped) {
            memcpy(ramped, data, len);
            // 淡入
            for (int i = 0; i < RAMP_SAMPLES; i++) {
                float gain = (float)i / (float)RAMP_SAMPLES;
                ramped[i] = (int16_t)(data[i] * gain);
            }
            // 淡出
            for (int i = 0; i < RAMP_SAMPLES; i++) {
                float gain = (float)(RAMP_SAMPLES - i) / (float)RAMP_SAMPLES;
                ramped[total_samples - RAMP_SAMPLES + i] =
                    (int16_t)(data[total_samples - RAMP_SAMPLES + i] * gain);
            }
            data = ramped;
        }
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(voice_tx_chan, data, len, &bytes_written, pdMS_TO_TICKS(1000));

    // 播放完毕后追加尾音静默（避免 I2S DMA 残留导致杂音）
    {
        int16_t tail[64] = {0};
        size_t tail_written = 0;
        for (int i = 0; i < 3; i++) {
            i2s_channel_write(voice_tx_chan, tail, sizeof(tail), &tail_written, pdMS_TO_TICKS(50));
        }
    }

    if (ramped) free(ramped);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "I2S write failed: %s", esp_err_to_name(ret));
    }

    // 播放结束，延迟一小段时间等尾巴发送完，然后静音功放
    vTaskDelay(pdMS_TO_TICKS(20));
    voice_speaker_mute();

    return ret;
}

esp_err_t voice_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!voice_tx_chan) return ESP_ERR_INVALID_STATE;
    if (freq_hz == 0 || duration_ms == 0) return ESP_ERR_INVALID_ARG;

    uint32_t num_samples = I2S_SAMPLE_RATE * duration_ms / 1000;
    int16_t *buf = (int16_t *)heap_caps_malloc(num_samples * sizeof(int16_t),
                                              MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = (int16_t *)heap_caps_malloc(num_samples * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "Tone buffer alloc failed (%lu bytes)", num_samples * sizeof(int16_t));
        return ESP_ERR_NO_MEM;
    }
    const float amplitude = 8000.0f;
    const float omega = 2.0f * (float)M_PI * (float)freq_hz / (float)I2S_SAMPLE_RATE;

    // 包络时长：5ms 起音 (attack)，剩余时长留 5ms 给释音 (release)
    uint32_t attack_samples  = (I2S_SAMPLE_RATE * 5) / 1000;   // 5ms
    uint32_t release_samples = (I2S_SAMPLE_RATE * 5) / 1000;   // 5ms
    if (attack_samples > num_samples / 4)   attack_samples  = num_samples / 4;
    if (release_samples > num_samples / 4)  release_samples = num_samples / 4;

    for (uint32_t i = 0; i < num_samples; i++) {
        // 正弦波
        float sample = sinf(omega * (float)i) * amplitude;

        // 包络（梯形：attack → 持续 → release）
        float env = 1.0f;
        if (i < attack_samples) {
            env = (float)i / (float)attack_samples;                // 线性淡入
        } else if (i >= num_samples - release_samples) {
            env = (float)(num_samples - i) / (float)release_samples; // 线性淡出
        }
        buf[i] = (int16_t)(sample * env);
    }

    voice_speaker_unmute();

    size_t bytes_written = 0;
    esp_err_t ret = i2s_channel_write(voice_tx_chan, buf, num_samples * sizeof(int16_t),
                                       &bytes_written, pdMS_TO_TICKS(duration_ms + 200));

    // 尾音静默
    {
        int16_t tail[64] = {0};
        size_t tail_written = 0;
        for (int i = 0; i < 3; i++) {
            i2s_channel_write(voice_tx_chan, tail, sizeof(tail), &tail_written, pdMS_TO_TICKS(50));
        }
    }

    free(buf);

    vTaskDelay(pdMS_TO_TICKS(20));
    voice_speaker_mute();

    return ret;
}

void *voice_get_rx_channel(void)
{
    return (void *)voice_rx_chan;
}

int voice_read_audio(int16_t *buf, int max_samples, TickType_t timeout)
{
    if (!g_audio_ring || !buf || max_samples <= 0) return -1;

    TickType_t deadline = xTaskGetTickCount() + timeout;
    int total = 0;

    while (total < max_samples) {
        portENTER_CRITICAL(&g_ring_lock);
        int avail = g_ring_write - g_ring_read;
        if (avail < 0) avail += VOICE_RING_SAMPLES;
        portEXIT_CRITICAL(&g_ring_lock);

        if (avail > 0) {
            int n = (avail < (max_samples - total)) ? avail : (max_samples - total);
            // 一次性拷贝，减少临界区时间
            int r_idx = g_ring_read;  // 快照
            for (int i = 0; i < n; i++) {
                buf[total + i] = g_audio_ring[(r_idx + i) % VOICE_RING_SAMPLES];
            }
            portENTER_CRITICAL(&g_ring_lock);
            g_ring_read = (r_idx + n) % VOICE_RING_SAMPLES;
            portEXIT_CRITICAL(&g_ring_lock);
            total += n;
        } else {
            if (timeout == 0) break;
            if (xTaskGetTickCount() >= deadline) break;
            vTaskDelay(pdMS_TO_TICKS(10));  // 必须≥1 tick，否则 IDLE 饥饿→TWDT 复位
        }
    }
    return total;
}
