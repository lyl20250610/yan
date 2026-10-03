#include "wifi.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "freertos/event_groups.h"
#include <string.h>
#include <stdbool.h>
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_FAST_RETRY_MAX      5      // 断开后先快速重连 5 次
#define WIFI_SLOW_RETRY_PERIOD_S 10     // 之后每 10s 后台重试，不再永久放弃
static EventGroupHandle_t s_wifi_event_group;
static const char *TAG = "WIFI";
static int s_retry_num = 0;
static TimerHandle_t s_retry_timer = NULL;

// 后台慢速重连：AP 长时间不可用时仍会周期性尝试，连上后由 GOT_IP 停表并清 FAIL 位
static void wifi_slow_retry_cb(TimerHandle_t xTimer)
{
    if (wifi_is_connected()) {
        return;
    }
    ESP_LOGI(TAG, "Background reconnect attempt");
    esp_wifi_connect();
}

static void wifi_start_slow_retry(void)
{
    if (s_retry_timer == NULL) {
        s_retry_timer = xTimerCreate("wifi_retry",
                                     pdMS_TO_TICKS(WIFI_SLOW_RETRY_PERIOD_S * 1000),
                                     pdTRUE, NULL, wifi_slow_retry_cb);
        if (s_retry_timer == NULL) {
            ESP_LOGE(TAG, "Create reconnect timer failed");
            return;
        }
    }
    if (xTimerIsTimerActive(s_retry_timer) != pdTRUE) {
        if (xTimerStart(s_retry_timer, 0) != pdPASS) {
            ESP_LOGW(TAG, "Start reconnect timer failed");
        }
    }
}

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < WIFI_FAST_RETRY_MAX) {
            s_retry_num++;
            ESP_LOGW(TAG, "Disconnected from AP, fast retry %d/%d",
                     s_retry_num, WIFI_FAST_RETRY_MAX);
            esp_wifi_connect();
        } else {
            // 告知初始化流程"暂时连不上"，让系统继续启动；
            // 同时转入后台慢速重连，避免设备永久失去网络
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            ESP_LOGW(TAG, "AP unavailable, retrying every %ds in background",
                     WIFI_SLOW_RETRY_PERIOD_S);
            wifi_start_slow_retry();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        if (s_retry_timer && xTimerIsTimerActive(s_retry_timer) == pdTRUE) {
            xTimerStop(s_retry_timer, 0);
        }
        xEventGroupClearBits(s_wifi_event_group, WIFI_FAIL_BIT);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init_sta(const char *ssid, const char *password)
{
    if (s_wifi_event_group == NULL) {
        s_wifi_event_group = xEventGroupCreate();
        if (s_wifi_event_group == NULL) {
            ESP_LOGE(TAG, "Create event group failed");
            return ESP_FAIL;
        }
    }
    // 清掉上一次的残留状态，重复调用时不会立刻返回旧结果
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_num = 0;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                        ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                        IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",
            .password = "",
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid) - 1] = '\0';
    strncpy((char*)wifi_config.sta.password, password, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password) - 1] = '\0';

    // memcpy(wifi_config.sta.ssid, ssid, strlen(ssid));
    // memcpy(wifi_config.sta.password, password, strlen(password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to AP");
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect");
        return ESP_FAIL;
    }
}

bool wifi_is_connected(void)
{
    wifi_mode_t mode;
    // 检查WiFi是否已经启动
    bool wifi_started = (esp_wifi_get_mode(&mode) == ESP_OK) && (mode != WIFI_MODE_NULL);
    
    return wifi_started && 
           esp_netif_is_netif_up(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));
}