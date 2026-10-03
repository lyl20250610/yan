#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_netif_sntp.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include <sys/time.h>
#include <time.h>


//各模块头文件
#include "bh1750.h"
#include "dht11.h"
#include "sgp30.h"
#include "apds9960.h"
#include "display.h"
#include "touch.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "wifi.h"
#include "mqtt.h"
#include "voice.h"
#include "ble.h"
#include "ui.h"
#include "xiaozi_ai.h"
#include "cJSON.h"

//定义 SGP30 全局句柄变量
sgp30_handle_t g_sgp30_handle = NULL;

//SPI总线引脚
#define SPI_SCK_PIN        GPIO_NUM_12
#define SPI_MOSI_PIN       GPIO_NUM_11
#define SPI_MISO_PIN       GPIO_NUM_13

//屏幕模块引脚
#define DISPLAY_CS_PIN     GPIO_NUM_10
#define DISPLAY_RST_PIN    GPIO_NUM_14
#define DISPLAY_DC_PIN     GPIO_NUM_15
#define DISPLAY_BL_PIN     GPIO_NUM_16
#define TOUCH_SDA_PIN      GPIO_NUM_6
#define TOUCH_SCL_PIN      GPIO_NUM_7
#define TOUCH_INT_PIN      GPIO_NUM_18
#define TOUCH_RST_PIN      GPIO_NUM_17

//DHT11温湿度传感器
#define DHT11_GPIO_PIN     GPIO_NUM_38

//APDS9960手势传感器
#define APDS9960_INT_PIN   GPIO_NUM_2

//AI按键（BOOT按键）
#define AI_BUTTON_GPIO     GPIO_NUM_0

//I2C总线
#define I2C_SENSORS_ENABLED 1
#define VOICE_ENABLED       1
#define XIAOZHI_LISTEN_MODE XIAOZHI_MODE_MANUAL
#define I2C_MASTER_NUM     I2C_NUM_0
#define I2C_MASTER_SDA_IO  GPIO_NUM_9
#define I2C_MASTER_SCL_IO  GPIO_NUM_8
#define I2C_MASTER_FREQ_HZ 100000
//MAX音频模块（I2S）
#define I2S_BCLK_PIN       GPIO_NUM_4
#define I2S_LRC_PIN        GPIO_NUM_5
#define I2S_DIN_PIN        GPIO_NUM_21
#define I2S_DOUT_PIN       GPIO_NUM_3

// 手势定义
#define GESTURE_UP          1
#define GESTURE_DOWN        2
#define GESTURE_LEFT        3
#define GESTURE_RIGHT       4
#define GESTURE_NEAR        5
#define GESTURE_FAR         6

// 事件类型
typedef enum {
    EVENT_GESTURE,
    EVENT_SENSOR_UPDATE,
    EVENT_WIFI_READY,
    EVENT_MQTT_READY
} event_type_t;

typedef struct {
    event_type_t type;
    union {
        int gesture_code;
        struct {
            float temperature;
            float humidity;
            float light;
            uint16_t tvoc;
            uint16_t co2;
        } sensor;
    } data;
} system_event_t;

static QueueHandle_t event_queue = NULL;
static const char *TAG = "MAIN";

// 传感器可用性标志
static bool g_bh1750_ok = false;
static bool g_sgp30_ok   = false;
static bool g_apds9960_ok = false;
static bool g_dht11_valid = false;

// 连续失败计数器（达到阈值后禁用，定期重试恢复）
#define SENSOR_MAX_FAIL        5     // 连续失败 5 次后禁用
#define SENSOR_RETRY_INTERVAL 30    // 每 30 个 1s 循环重试恢复
#define MQTT_REPORT_INTERVAL 2      // 每 2s 上报一次云端
static int g_bh1750_fail_count = 0;
static int g_sgp30_fail_count   = 0;
static bool g_voice_ready       = false;
static bool g_onenet_started    = false;
static bool g_xiaozhi_started   = false;
static bool g_sntp_started      = false;
static bool g_sgp30_baseline_restored = false;

static void xiaozhi_event_handler(xiaozhi_event_t event, const char *data,
                                  void *user_data);

// 外部变量及函数声明
extern sgp30_handle_t g_sgp30_handle;



// 属性查询回调：平台查询设备当前属性时触发
static void onenet_query_handler(const char *msg_id)
{
    ESP_LOGI(TAG, "Property query received, msg_id=%s", msg_id);

    float temp = g_dht11_valid ? dht11_get_temperature() : -99.0f;
    float humi = g_dht11_valid ? dht11_get_humidity() : -99.0f;
    float lux  = g_bh1750_ok ? bh1750_get_lux() : 0.0f;
    uint16_t tvoc = (g_sgp30_ok && g_sgp30_handle) ? sgp30_get_tvoc() : 0;
    uint16_t co2  = (g_sgp30_ok && g_sgp30_handle) ? sgp30_get_co2() : 0;

    char params[384];
    snprintf(params, sizeof(params),
             "\"temperature\":{\"value\":%.1f},"
             "\"humidity\":{\"value\":%.1f},"
             "\"light\":{\"value\":%.1f},"
             "\"tvoc\":{\"value\":%d},"
             "\"CO2\":{\"value\":%d}",
             temp, humi, lux, tvoc, co2);

    esp_err_t qr_err = onenet_property_query_reply(msg_id, params);
    if (qr_err != ESP_OK) {
        ESP_LOGE(TAG, "Query reply failed: %s (MQTT %s)",
                 esp_err_to_name(qr_err),
                 onenet_is_connected() ? "connected" : "DISCONNECTED");
    }
}

static bool parse_screen_value(const char *json, int len, int *value)
{
    if (!json || len <= 0 || !value) return false;
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!root) return false;

    cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    cJSON *screen = params ? cJSON_GetObjectItemCaseSensitive(params, "screen") : NULL;
    cJSON *item = cJSON_IsNumber(screen)
                    ? screen
                    : (screen ? cJSON_GetObjectItemCaseSensitive(screen, "value") : NULL);
    bool ok = cJSON_IsNumber(item);
    if (ok) *value = item->valueint;
    cJSON_Delete(root);
    return ok;
}

/** 属性设置回调：目前支持 screen 背光亮度（0~255） */
static void onenet_property_set_handler(const char *msg_id,
                                         const char *params_json, int data_len)
{
    ESP_LOGI(TAG, "Property set received [%s]: %.*s", msg_id, data_len, params_json);
    int value = 0;
    if (parse_screen_value(params_json, data_len, &value)) {
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        display_backlight_set(value);
        onenet_property_set_reply(msg_id, 200, "success");
    } else {
        onenet_property_set_reply(msg_id, 400, "unsupported property");
    }
}

/** 服务调用回调：平台调用设备服务时触发 */
static void onenet_service_handler(const char *msg_id, const char *service_id,
                                    const char *params_json, int data_len)
{
    ESP_LOGI(TAG, "Service invoke [%s] service=%s data=%.*s",
             msg_id, service_id, data_len, params_json);
    int value = 0;
    if (strcmp(service_id, "screen") == 0 &&
        parse_screen_value(params_json, data_len, &value)) {
        if (value < 0) value = 0;
        if (value > 255) value = 255;
        display_backlight_set(value);
        onenet_service_reply(service_id, msg_id, 200, "success");
    } else {
        onenet_service_reply(service_id, msg_id, 400, "unsupported service");
    }
}




//BLE 控制回调

static void ble_control_handler(const char *cmd)
{
    ESP_LOGI(TAG, "BLE control: %s", cmd);
    if (strncmp(cmd, "light:", 6) == 0) {
        ESP_LOGW(TAG, "BLE light command ignored: no light actuator is configured");
        ui_update_ai_reply("灯光控制未配置");
    } else if (strncmp(cmd, "screen:", 7) == 0) {
        int val = atoi(cmd + 7);
        display_backlight_set(val);
    }
}

static void voice_wake_handler(void)
{
    esp_err_t err = xiaozhi_ai_wake_word_detected("你好小智");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Wake event ignored: %s", esp_err_to_name(err));
    }
}

static esp_err_t start_xiaozhi(void)
{
    if (g_xiaozhi_started) return ESP_OK;
    if (!wifi_is_connected() || !g_voice_ready) return ESP_ERR_INVALID_STATE;

    xiaozhi_ai_config_t cfg = {
        .server_url   = NULL,
        .access_token = NULL,
        .mode         = XIAOZHI_LISTEN_MODE,
        .event_cb     = xiaozhi_event_handler,
        .user_data    = NULL,
    };
    esp_err_t err = xiaozhi_ai_init(&cfg);
    if (err == ESP_OK) {
        g_xiaozhi_started = true;
        xiaozhi_ai_mute_tts(false);
        ui_update_ai_status("小智连接中...");
        ESP_LOGI(TAG, "小智AI已启动 (wake-word mode, TTS enabled)");
    }
    return err;
}

static bool system_time_is_valid(void)
{
    time_t now = 0;
    time(&now);
    return now >= 1704067200;  // 2024-01-01 UTC
}

static void ensure_network_time(void)
{
    if (!g_sntp_started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
            ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com", "cn.ntp.org.cn"));
        cfg.server_from_dhcp = false;
        cfg.wait_for_sync = true;
        cfg.start = true;
        esp_err_t err = esp_netif_sntp_init(&cfg);
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
            g_sntp_started = true;
        } else {
            ESP_LOGW(TAG, "SNTP recovery init failed: %s", esp_err_to_name(err));
            return;
        }
    }
    if (!system_time_is_valid()) {
        esp_err_t err = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "SNTP recovery sync pending: %s", esp_err_to_name(err));
        }
    }
}

static esp_err_t start_onenet(void)
{
    onenet_register_property_query_callback(onenet_query_handler);
    onenet_register_property_set_callback(onenet_property_set_handler);
    onenet_register_service_callback(onenet_service_handler);
    esp_err_t err = onenet_start();
    if (err == ESP_OK) g_onenet_started = true;
    return err;
}

static void network_watchdog_task(void *arg)
{
    bool last_wifi = false;
    bool last_mqtt = false;
    while (1) {
        bool wifi_ok = wifi_is_connected();
        bool mqtt_ok = onenet_is_connected();
        if (wifi_ok != last_wifi) {
            ui_update_wifi_status(wifi_ok);
            ble_set_wifi_status(wifi_ok);
            last_wifi = wifi_ok;
        }
        if (mqtt_ok != last_mqtt) {
            ui_update_mqtt_status(mqtt_ok);
            last_mqtt = mqtt_ok;
        }

        if (wifi_ok) {
            ensure_network_time();
            if (!g_onenet_started && system_time_is_valid()) {
                esp_err_t err = start_onenet();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "OneNET recovery start failed: %s", esp_err_to_name(err));
                }
            }
            if (!g_xiaozhi_started && g_voice_ready) {
                esp_err_t err = start_xiaozhi();
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "小智AI恢复启动失败: %s", esp_err_to_name(err));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// 小智AI事件回调 —— 更新UI状态
static void xiaozhi_event_handler(xiaozhi_event_t event, const char *data, void *user_data)
{
    switch (event) {
        case XIAOZHI_EVENT_CONNECTED:
            ESP_LOGI(TAG, "小智AI: 已连接");
            ui_update_ai_status("小智在线");
            break;
        case XIAOZHI_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "小智AI: 已断开");
            ui_update_ai_status("小智离线");
            break;
        case XIAOZHI_EVENT_LISTENING:
            ESP_LOGI(TAG, "小智AI: 聆听中...");
            ui_update_ai_status("聆听中...");
            break;
        case XIAOZHI_EVENT_THINKING:
            ESP_LOGI(TAG, "小智AI: 思考中...");
            ui_update_ai_status("思考中...");
            break;
        case XIAOZHI_EVENT_SPEAKING:
            ESP_LOGI(TAG, "小智AI: 回复中...");
            ui_update_ai_status("回复中...");
            break;
        case XIAOZHI_EVENT_ASR_TEXT:
            if (data) {
                ESP_LOGI(TAG, "小智ASR: %s", data);
                ui_update_ai_reply(data);
            }
            break;
        case XIAOZHI_EVENT_TTS_START:
        case XIAOZHI_EVENT_TTS_STOP:
            break;
        case XIAOZHI_EVENT_LLM_TEXT:
            if (data) {
                ESP_LOGI(TAG, "小智LLM: %s", data);
                ui_chat_add_message(data, false);  
                ui_update_ai_reply(data);         
            }
            break;
        case XIAOZHI_EVENT_ERROR:
            ESP_LOGW(TAG, "小智AI错误: %s", data ? data : "unknown");
            {
                const char *activation_code = xiaozhi_ai_get_activation_code();
                if (activation_code) {
                    char prompt[48];
                    snprintf(prompt, sizeof(prompt), "设备激活码: %s", activation_code);
                    ui_update_ai_status(prompt);
                    ui_update_ai_reply(prompt);
                } else {
                    ui_update_ai_status("小智出错");
                }
            }
            break;
    }
}

// 聊天发送回调
static void chat_send_handler(const char *text)
{
    if (!text || !text[0]) return;
    ESP_LOGI(TAG, "Chat send: %s", text);
    ui_chat_add_message(text, true);   
    esp_err_t err = xiaozhi_ai_send_text(text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Chat send failed: %s", esp_err_to_name(err));
        ui_update_ai_status("消息发送失败");
    }
}

//按键任务 —— 短按亮屏
static void ai_button_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << AI_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    bool last_state = true;
    while (1) {
        bool current = gpio_get_level(AI_BUTTON_GPIO);
        if (last_state && !current) {  // 按下
            ESP_LOGI(TAG, "按键按下");
            display_backlight_set(255);  // 亮屏
        }
        last_state = current;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static bool save_sgp30_baseline(void)
{
    if (!g_sgp30_ok || !g_sgp30_handle) return false;

    uint16_t co2_base = 0;
    uint16_t tvoc_base = 0;
    if (sgp30_get_baseline(g_sgp30_handle, &co2_base, &tvoc_base) != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 baseline read failed");
        return false;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("sgp30", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return false;
    err = nvs_set_u16(nvs, "co2_base", co2_base);
    if (err == ESP_OK) err = nvs_set_u16(nvs, "tvoc_base", tvoc_base);
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SGP30 baseline save failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "SGP30 baseline saved");
    return true;
}

// 传感器数据更新及 MQTT 上报任务
static void sensor_mqtt_task(void *arg) {
    system_event_t evt;
    evt.type = EVENT_SENSOR_UPDATE;
    unsigned mqtt_report_tick = 0;
    uint32_t baseline_age_sec = 0;
    uint32_t baseline_save_after_sec = g_sgp30_baseline_restored ? 3600U : 43200U;
    TickType_t last_wake = xTaskGetTickCount();

    while (1) {
        // 传感器更新
        // BH1750
        if (g_bh1750_ok) {
            if (bh1750_update() != ESP_OK) {
                g_bh1750_fail_count++;
                ESP_LOGW(TAG, "BH1750 read fail (%d/%d)", g_bh1750_fail_count, SENSOR_MAX_FAIL);
                if (g_bh1750_fail_count >= SENSOR_MAX_FAIL) {
                    g_bh1750_ok = false;
                    ESP_LOGW(TAG, "BH1750 disabled, will retry later");
                }
            } else {
                g_bh1750_fail_count = 0;  // 成功则清零
            }
        } else {
            static int bh1750_retry_tick = 0;
            if (++bh1750_retry_tick >= SENSOR_RETRY_INTERVAL) {
                bh1750_retry_tick = 0;
                ESP_LOGI(TAG, "Retrying BH1750 reinit...");
                if (bh1750_reinit() == ESP_OK) {
                    g_bh1750_ok = true;
                    g_bh1750_fail_count = 0;
                    ESP_LOGI(TAG, "BH1750 recovered!");
                }
            }
        }
        // SGP30
        if (g_sgp30_ok && g_sgp30_handle) {
            if (sgp30_update(g_sgp30_handle) != ESP_OK) {
                g_sgp30_fail_count++;
                ESP_LOGW(TAG, "SGP30 read fail (%d/%d)", g_sgp30_fail_count, SENSOR_MAX_FAIL);
                if (g_sgp30_fail_count >= SENSOR_MAX_FAIL) {
                    g_sgp30_ok = false;
                    ESP_LOGW(TAG, "SGP30 disabled, will retry later");
                }
            } else {
                g_sgp30_fail_count = 0;  // 成功则清零
                baseline_age_sec++;
                if (baseline_age_sec >= baseline_save_after_sec &&
                    save_sgp30_baseline()) {
                    baseline_age_sec = 0;
                    baseline_save_after_sec = 3600U;
                }
            }
        } else {
           
            static int sgp30_retry_tick = 0;
            if (++sgp30_retry_tick >= SENSOR_RETRY_INTERVAL) {
                sgp30_retry_tick = 0;
                if (g_sgp30_handle != NULL) {
                    ESP_LOGI(TAG, "Retrying SGP30 reinit...");
                    if (sgp30_init(g_sgp30_handle) == ESP_OK) {
                        g_sgp30_ok = true;
                        g_sgp30_fail_count = 0;
                        ESP_LOGI(TAG, "SGP30 recovered!");
                    }
                }
            }
        }

        float temp = dht11_get_temperature();
        float humi = dht11_get_humidity();
        bool dht_valid_now = dht11_is_valid();
        if (dht_valid_now && !g_dht11_valid) {
            ESP_LOGI(TAG, "DHT11 first valid reading: T=%.1f°C H=%.1f%%", temp, humi);
        }
        g_dht11_valid = dht_valid_now;
        evt.data.sensor.temperature = g_dht11_valid ? temp : -99.0f;
        evt.data.sensor.humidity    = g_dht11_valid ? humi : -99.0f;


        // 读取真实传感器数值
        evt.data.sensor.light = g_bh1750_ok ? bh1750_get_lux() : 0.0f;
        evt.data.sensor.tvoc  = (g_sgp30_ok && g_sgp30_handle) ? sgp30_get_tvoc() : 0;
        evt.data.sensor.co2   = (g_sgp30_ok && g_sgp30_handle) ? sgp30_get_co2() : 0;

        // MQTT 属性上报
        mqtt_report_tick++;
        if (mqtt_report_tick >= MQTT_REPORT_INTERVAL && onenet_is_connected() &&
            xiaozhi_ai_is_ready()) {
            mqtt_report_tick = 0;
            char params[384];
            snprintf(params, sizeof(params),
                     "\"temperature\":{\"value\":%.1f},"
                     "\"humidity\":{\"value\":%.1f},"
                     "\"light\":{\"value\":%.1f},"
                     "\"tvoc\":{\"value\":%d},"
                     "\"CO2\":{\"value\":%d}",
                     evt.data.sensor.temperature,
                     evt.data.sensor.humidity,
                     evt.data.sensor.light,
                     evt.data.sensor.tvoc,
                     evt.data.sensor.co2);
            esp_err_t rpt_err = onenet_report_properties(params);
            if (rpt_err != ESP_OK) {
                ESP_LOGW(TAG, "Property report failed: %s",
                         esp_err_to_name(rpt_err));
            }
        }

        // 更新 BLE 特征值
        ble_update_temperature(evt.data.sensor.temperature);
        ble_update_humidity(evt.data.sensor.humidity);
        ble_update_light(evt.data.sensor.light);
        ble_update_tvoc(evt.data.sensor.tvoc);
        ble_update_co2(evt.data.sensor.co2);

        //更新UI显示
        ui_update_sensors(evt.data.sensor.temperature,
                          evt.data.sensor.humidity,
                          evt.data.sensor.light,
                          evt.data.sensor.tvoc,
                          evt.data.sensor.co2);

        xQueueSend(event_queue, &evt, 0);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(1000));
    }
}

// 手势检测任务
static void gesture_task(void *arg) {
#if I2C_SENSORS_ENABLED
    uint8_t gesture = 0;
    system_event_t evt;
    evt.type = EVENT_GESTURE;
#endif

    // 初始化APDS9960中断引脚
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << APDS9960_INT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);

    while (1) {
#if I2C_SENSORS_ENABLED
        if (g_apds9960_ok) {
            if (apds9960_read_gesture(&gesture) == ESP_OK && gesture != 0) {
                evt.data.gesture_code = gesture;
                xQueueSend(event_queue, &evt, 0);
                gesture = 0;  // 清除手势状态
            }
        }
#endif
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// 事件处理任务（处理手势、传感器更新等）
static void event_processor_task(void *arg) {
    system_event_t evt;

    while (1) {
        if (xQueueReceive(event_queue, &evt, portMAX_DELAY) == pdTRUE) {
            switch (evt.type) {
                case EVENT_GESTURE:
                    ESP_LOGI(TAG, "Gesture code: %d", evt.data.gesture_code);
                    switch (evt.data.gesture_code) {
                        case GESTURE_UP:
                        case GESTURE_RIGHT:
                            ui_switch_to_next_page();
                            break;
                        case GESTURE_DOWN:
                        case GESTURE_LEFT:
                            ui_switch_to_prev_page();
                            break;
                        case GESTURE_NEAR:
                        case GESTURE_FAR:
                            // 亮屏/息屏已取消
                            break;
                        default:
                            break;
                    }
                    break;

                case EVENT_SENSOR_UPDATE:
                    //
                    break;

                default:
                    break;
            }
        }
    }
}

// LVGL 任务
extern void lvgl_port_task(void *arg);

// 主函数
void app_main(void) {
    esp_err_t ret;
    ESP_LOGI(TAG, "System starting...");

    //初始化 NVS
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化SPI总线
    spi_bus_config_t spi_bus_cfg = {
        .miso_io_num = SPI_MISO_PIN,
        .mosi_io_num = SPI_MOSI_PIN,
        .sclk_io_num = SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(lv_color_t)  // 给DMA描述符充足余量，避免对齐导致ESP_ERR_NO_MEM
    };
    ret = spi_bus_initialize(SPI2_HOST, &spi_bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "SPI bus initialized (SPI2_HOST)");

    //初始化显示、触摸、LVGL、UI
    display_pin_cfg_t display_pins = {
        .cs_pin  = DISPLAY_CS_PIN,
        .dc_pin  = DISPLAY_DC_PIN,
        .rst_pin = DISPLAY_RST_PIN,
        .bl_pin  = DISPLAY_BL_PIN,
    };
    ESP_ERROR_CHECK(display_init(&display_pins));
    touch_pin_cfg_t touch_pins = {
        .sda_pin = TOUCH_SDA_PIN,
        .scl_pin = TOUCH_SCL_PIN,
        .int_pin = TOUCH_INT_PIN,
        .rst_pin = TOUCH_RST_PIN,
    };
    ESP_ERROR_CHECK(touch_init(&touch_pins));
    ESP_ERROR_CHECK(lvgl_port_init());
    ui_init();
    ui_chat_set_send_callback(chat_send_handler);
    ESP_LOGI(TAG, "Display & LVGL & UI ready");

    //初始化 I2C 总线
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_MASTER_SDA_IO,
        .scl_io_num = I2C_MASTER_SCL_IO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
    };
    ESP_ERROR_CHECK(i2c_param_config(I2C_MASTER_NUM, &conf));
    ESP_ERROR_CHECK(i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0));
    ESP_LOGI(TAG, "I2C bus initialized (I2C_NUM_0)");

    //初始化传感器
    dht11_init(DHT11_GPIO_PIN);
    dht11_start_task();
    vTaskDelay(pdMS_TO_TICKS(100));  // 让 DHT11 任务完成首次读取
    ESP_LOGI(TAG, "DHT11 initialized (GPIO%d)", DHT11_GPIO_PIN);

#if I2C_SENSORS_ENABLED
    // I2C 总线扫描：检测已连接的设备，避免对不存在的传感器重复发送命令
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "Scanning I2C bus (I2C_NUM_0)...");
    int i2c_devices_found = 0;
    bool found_0x23 = false, found_0x5C = false, found_0x58 = false, found_0x39 = false;
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            i2c_devices_found++;
            ESP_LOGI(TAG, "  Found device at 0x%02X", addr);
            if (addr == 0x23) found_0x23 = true;
            if (addr == 0x5C) found_0x5C = true;
            if (addr == 0x58) found_0x58 = true;
            if (addr == 0x39) found_0x39 = true;
        }
    }
    ESP_LOGI(TAG, "I2C scan done: %d device(s) found", i2c_devices_found);

    if (i2c_devices_found == 0) {
        ESP_LOGW(TAG, "No I2C devices on GPIO%d(SCL)/GPIO%d(SDA) — check wiring/power/pull-ups",
                 I2C_MASTER_SCL_IO, I2C_MASTER_SDA_IO);
    } else {
        if (found_0x23 || found_0x5C) {
            uint8_t bh_addr = found_0x23 ? 0x23 : 0x5C;
            g_bh1750_ok = (bh1750_init(I2C_MASTER_NUM, bh_addr) == ESP_OK);
            if (g_bh1750_ok) ESP_LOGI(TAG, "BH1750 initialized (addr=0x%02X)", bh_addr);
        } else {
            ESP_LOGW(TAG, "BH1750 (0x23/0x5C) not found on I2C bus, skipping");
        }

        if (found_0x58) {
            g_sgp30_handle = sgp30_create(I2C_MASTER_NUM);
            if (g_sgp30_handle) {
                g_sgp30_ok = (sgp30_init(g_sgp30_handle) == ESP_OK);
                if (g_sgp30_ok) {
                    nvs_handle_t nvs_handle;
                    if (nvs_open("sgp30", NVS_READONLY, &nvs_handle) == ESP_OK) {
                        uint16_t co2_base = 0, tvoc_base = 0;
                        esp_err_t co2_err = nvs_get_u16(nvs_handle, "co2_base", &co2_base);
                        esp_err_t tvoc_err = nvs_get_u16(nvs_handle, "tvoc_base", &tvoc_base);
                        nvs_close(nvs_handle);
                        if (co2_err == ESP_OK && tvoc_err == ESP_OK) {
                            if (sgp30_restore_baseline(g_sgp30_handle, co2_base,
                                                       tvoc_base) == ESP_OK) {
                                g_sgp30_baseline_restored = true;
                                ESP_LOGI(TAG, "SGP30 baseline restored");
                            }
                        } else {
                            ESP_LOGI(TAG, "No valid SGP30 baseline in NVS; starting fresh");
                        }
                    }
                    ESP_LOGI(TAG, "SGP30 initialized");
                }
            } else {
                ESP_LOGW(TAG, "SGP30 create failed");
            }
        } else {
            ESP_LOGW(TAG, "SGP30 (0x58) not found on I2C bus, skipping");
        }

        if (found_0x39) {
            g_apds9960_ok = (apds9960_init(I2C_MASTER_NUM) == ESP_OK);
            if (g_apds9960_ok) ESP_LOGI(TAG, "APDS9960 initialized (INT: GPIO%d)", APDS9960_INT_PIN);
        } else {
            ESP_LOGW(TAG, "APDS9960 (0x39) not found on I2C bus, skipping");
        }
    }
#endif

    // 7. 连接 Wi-Fi 并启动 MQTT
    ret = wifi_init_sta(WIFI_SSID, WIFI_PASSWORD);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wi-Fi connected, waiting for network ready...");
        ble_set_wifi_status(true);
        {
            esp_netif_t *sta_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta_netif) {
                esp_netif_dns_info_t dns;
                memset(&dns, 0, sizeof(dns));
                dns.ip.type = IPADDR_TYPE_V4;

                // 检查 DHCP 是否已提供 DNS
                esp_netif_dns_info_t current_dns;
                bool dhcp_has_dns = false;
                if (esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &current_dns) == ESP_OK) {
                    if (current_dns.ip.u_addr.ip4.addr != 0 &&
                        current_dns.ip.u_addr.ip4.addr != IPADDR_NONE) {
                        dhcp_has_dns = true;
                        ESP_LOGI(TAG, "DHCP provided DNS: " IPSTR,
                                 IP2STR(&current_dns.ip.u_addr.ip4));
                    }
                }

                if (!dhcp_has_dns) {
                    dns.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
                    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif,
                        ESP_NETIF_DNS_MAIN, &dns));
                    ESP_LOGI(TAG, "DNS MAIN set: 114.114.114.114 (DHCP gave none)");

                    dns.ip.u_addr.ip4.addr = ipaddr_addr("8.8.8.8");
                    ESP_ERROR_CHECK(esp_netif_set_dns_info(sta_netif,
                        ESP_NETIF_DNS_BACKUP, &dns));
                    ESP_LOGI(TAG, "DNS BACKUP set: 8.8.8.8");
                } else {
                    // DHCP 给了 DNS —— 仅追加 FALLBACK 作为保险
                    dns.ip.u_addr.ip4.addr = ipaddr_addr("114.114.114.114");
                    esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_FALLBACK, &dns);
                    ESP_LOGI(TAG, "DNS FALLBACK set: 114.114.114.114");
                }
            } else {
                ESP_LOGW(TAG, "Cannot get STA netif handle — DNS may fail");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
        {
            esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(3,
                ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "ntp.tencent.com", "cn.ntp.org.cn"));
            sntp_cfg.server_from_dhcp = false;
            sntp_cfg.wait_for_sync = true;
            sntp_cfg.start = true;
            esp_err_t sntp_ret = esp_netif_sntp_init(&sntp_cfg);
            if (sntp_ret == ESP_OK) {
                g_sntp_started = true;
                ESP_LOGI(TAG, "SNTP started with 3 servers, waiting for time sync...");
                sntp_ret = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(20000));  // 20秒超时
                if (sntp_ret == ESP_OK) {
                    ESP_LOGI(TAG, "NTP time synced successfully");
                    setenv("TZ", "CST-8", 1);
                    tzset();
                } else {
                    ESP_LOGW(TAG, "NTP time sync failed (%s), using fallback time", esp_err_to_name(sntp_ret));
                    {
                        struct tm tm_fb = {0};
                        char month_str[4];
                        int day, year;
                        sscanf(__DATE__, "%3s %d %d", month_str, &day, &year);
                        const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
                        const char *pos = strstr(months, month_str);
                        if (pos) tm_fb.tm_mon = (pos - months) / 3;
                        tm_fb.tm_mday = day;
                        tm_fb.tm_year = year - 1900;
                        tm_fb.tm_hour = 12;  
                        time_t fallback_time = mktime(&tm_fb);
                        if (fallback_time > 0) {
                            struct timeval tv = { .tv_sec = fallback_time, .tv_usec = 0 };
                            settimeofday(&tv, NULL);
                            ESP_LOGW(TAG, "Using compile time fallback: %s", __DATE__);
                        }
                    }
                }
            } else {
                ESP_LOGW(TAG, "SNTP init failed (%s), MQTT may fail", esp_err_to_name(sntp_ret));
            }
        }


        vTaskDelay(pdMS_TO_TICKS(1000));


        ESP_LOGI(TAG, "DNS pre-check for MQTT broker...");
        {
            struct addrinfo hints = {
                .ai_family = AF_INET,
                .ai_socktype = SOCK_STREAM,
            };
            struct addrinfo *res = NULL;
            int gai_err = 0;
            for (int retry = 0; retry < 5; retry++) {
                res = NULL;
                gai_err = getaddrinfo("studio-mqtts.heclouds.com", "8883", &hints, &res);
                if (gai_err == 0 && res) break;
                if (res) { freeaddrinfo(res); res = NULL; }
                ESP_LOGW(TAG, "DNS retry %d/5: err %d", retry + 1, gai_err);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            if (gai_err == 0 && res) {
                char ip_str[INET_ADDRSTRLEN];
                struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &addr->sin_addr, ip_str, sizeof(ip_str));
                ESP_LOGI(TAG, "DNS OK: studio-mqtts.heclouds.com → %s", ip_str);
                freeaddrinfo(res);
            } else {
                ESP_LOGW(TAG, "DNS FAIL after 5 retries: err %d", gai_err);
            }
        }

        ESP_LOGI(TAG, "Initializing OneNET MQTT...");
        ui_update_wifi_status(true);
        esp_err_t mqtt_start_err = start_onenet();
        if (mqtt_start_err != ESP_OK) {
            ESP_LOGW(TAG, "OneNET start failed: %s", esp_err_to_name(mqtt_start_err));
        }

        ESP_LOGI(TAG, "Waiting for MQTT TLS handshake...");
        for (int i = 0; i < 50; i++) {
            if (onenet_is_connected()) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (onenet_is_connected()) {
            ESP_LOGI(TAG, "MQTT connected");
            ui_update_mqtt_status(true);
        } else {
            ESP_LOGW(TAG, "MQTT not connected after 5s");
            ui_update_mqtt_status(false);
        }
    } else {
        ESP_LOGE(TAG, "Wi-Fi connection failed, MQTT not started");
        ui_update_wifi_status(false);
    }

#if VOICE_ENABLED
    voice_pin_cfg_t voice_pins = {
        .bclk_pin   = I2S_BCLK_PIN,
        .ws_pin     = I2S_LRC_PIN,
        .din_pin    = I2S_DIN_PIN,
        .dout_pin   = I2S_DOUT_PIN,
        .spk_sd_pin = GPIO_NUM_1,
    };
    ret = voice_io_init(&voice_pins);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Voice I/O init failed");
    } else {
        voice_set_wake_callback(voice_wake_handler);
        ret = voice_module_init();
        if (ret == ESP_OK) {
            g_voice_ready = true;
            ESP_LOGI(TAG, "Voice I/O, wake word and microphone pipeline ready");
        } else {
            ESP_LOGW(TAG, "Voice recognition unavailable: %s", esp_err_to_name(ret));
        }
    }
#else
    ESP_LOGI(TAG, "Voice module disabled");
#endif

    {
        time_t now;
        struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);
        ESP_LOGI(TAG, "System time: %04d-%02d-%02d %02d:%02d:%02d (TLS needs correct time)",
                 timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                 timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    // 小智 AI：Wi-Fi 已就绪时立即启动，否则由网络守护任务恢复启动。
    esp_err_t xz_ret = start_xiaozhi();
    if (xz_ret != ESP_OK) {
        ESP_LOGW(TAG, "小智AI暂未启动: %s (WiFi=%s Voice=%s)",
                 esp_err_to_name(xz_ret),
                 wifi_is_connected() ? "OK" : "DOWN",
                 g_voice_ready ? "OK" : "DOWN");
        ui_update_ai_status("小智等待网络/语音");
    }

    // BLE 外设 —— 传感器数据 + 设备控制
    ble_init();
    ble_set_control_callback(ble_control_handler);
    ESP_LOGI(TAG, "BLE GATT Server ready");

    // 11. 创建事件队列（用于手势、传感器事件）
    event_queue = xQueueCreate(10, sizeof(system_event_t));
    if (!event_queue) {
        ESP_LOGE(TAG, "Event queue create failed");
        return;
    }

    // 12. 创建 FreeRTOS 任务
    bool tasks_ok = true;
    tasks_ok &= (xTaskCreatePinnedToCore(sensor_mqtt_task, "sensor_mqtt", 4096, NULL, 2, NULL, 1) == pdPASS);
    tasks_ok &= (xTaskCreatePinnedToCore(gesture_task, "gesture", 3072, NULL, 3, NULL, 1) == pdPASS);
    tasks_ok &= (xTaskCreatePinnedToCore(event_processor_task, "evt_proc", 4096, NULL, 4, NULL, 1) == pdPASS);
    tasks_ok &= (xTaskCreatePinnedToCore(ai_button_task, "ai_button", 4096, NULL, 3, NULL, 1) == pdPASS);
    tasks_ok &= (xTaskCreatePinnedToCore(network_watchdog_task, "net_watch", 4096, NULL, 2, NULL, 0) == pdPASS);
    if (!tasks_ok) {
        ESP_LOGE(TAG, "One or more application tasks failed to start");
    }

    ESP_LOGI(TAG, "System ready, all tasks started!");
    ESP_LOGI(TAG, "LVGL running in main loop (no separate task)");


    int lvgl_loop = 0;
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        lvgl_loop++;
        if (lvgl_loop <= 3) {
            ESP_LOGI(TAG, "LVGL loop #%d: handler returned %lu ms",
                     lvgl_loop, (unsigned long)delay_ms);
        }
        if (delay_ms < 10) delay_ms = 10;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
