#include "mqtt.h"
#include "mqtt_client.h"
#include "esp_log.h"
#include "onenet_token.h"
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ONENET";

/* ---- 重连退避 ---- */
static esp_timer_handle_t s_reconnect_timer = NULL;
static int s_backoff_sec = 5;           /* 初始退避 5 秒 */
static int s_reconnect_count = 0;       /* 连续失败计数 */
static const int BACKOFF_MAX_SEC = 120; /* 最大退避 120 秒 */
static char s_token[256];               /* token 持久化，重连时可重新生成 */

static const char ONENET_CA_CERT[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDNTCCAh2gAwIBAgIJAI0j4gYGUcD2MA0GCSqGSIb3DQEBCwUAMDExCzAJBgNV\n"
    "BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDESMBAGA1UEAwwJT25lTkVUIE1RMB4XDTE5\n"
    "MDYxMzAyMTgzM1oXDTQ5MDYwNTAyMTgzM1owMTELMAkGA1UEBhMCQ04xDjAMBgNV\n"
    "BAoMBUNNSU9UMRIwEAYDVQQDDAlPbmVORVQgTVEwggEiMA0GCSqGSIb3DQEBAQUA\n"
    "A4IBDwAwggEKAoIBAQDrSdCdWHPwgB1KFo6Gb3nbjVYMbx+3hQ1Qdf+M4+i8MgYz\n"
    "KSdWTvBf5+eSDEPhAMf7CDbfLtqpEKJPQ8MqcqwRFn5Ahux+bfgzV8QHwsbxqkPz\n"
    "Z8Ga/2PkL3fJM1Cunq1o4WNRNwgWI0JQbqwEofhuUasJuq63jE7bdzIE60eEjsFw\n"
    "Tc6ZiRZf9DzQDfI3v0wOKTTfyUlJZgBpQouJBFio9cMW15DvqcT7VmDHd9+gppBa\n"
    "Wq6Nak+EverpyWW8TaqnwIQRdRwxXu6f8iNQb5UlgW2cUP7pZu0xIY+sE4E94mpV\n"
    "hZHEmbO3Y8x9Q1r5YwG0oTz+UfCI+toh9OIV2msjAgMBAAGjUDBOMB0GA1UdDgQW\n"
    "BBSBOGqKT3bVuY6bYlaCfEwryMarIzAfBgNVHSMEGDAWgBSBOGqKT3bVuY6bYlaC\n"
    "fEwryMarIzAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBwSZz/bbpF\n"
    "yNudik/ZkVIiGkTDC0iPQruZxEghX/UMVSJEpOMOOTz9ws5WUb7B3XWRk85L0YRC\n"
    "Vh9Axcs7zAiPsVVvPmYO0GkWYzP6MedLyXHMLuoqHSRC6NlgyITXGe7vrClF7YSG\n"
    "C0nStWF1kQ48RX17Ty5gCsIO/21PIVeXxDXWNhMGs22cLGUR2udGwJQdG5q0ZJwY\n"
    "hsVDcAQCOP2nuGivh6cP+2acXQH+6C1KMWV4vIk63cC4G1VgXV4ai+Glf6riA8jO\n"
    "txrtp4v0uwUerArO+wtX/2YXaQWbhYs5+K0dOIi8TltVWiXxyXKqYSsjFbSq0h7F\n"
    "KZ2I6wtffJnH\n"
    "-----END CERTIFICATE-----\n"
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDOzCCAiOgAwIBAgIJAPCCNfxANtVEMA0GCSqGSIb3DQEBCwUAMDQxCzAJBgNV\n"
    "BAYTAkNOMQ4wDAYDVQQKDAVDTUlPVDEVMBMGA1UEAwwMT25lTkVUIE1RVFRTMB4X\n"
    "DTE5MDUyOTAxMDkyOFoXDTQ5MDUyMTAxMDkyOFowNDELMAkGA1UEBhMCQ04xDjAM\n"
    "BgNVBAoMBUNNSU9UMRUwEwYDVQQDDAxPbmVORVQgTVFUVFMwggEiMA0GCSqGSIb3\n"
    "DQEBAQUAA4IBDwAwggEKAoIBAQC/VvJ6lGWfy9PKdXKBdzY83OERB35AJhu+9jkx\n"
    "5d4SOtZScTe93Xw9TSVRKrFwu5muGgPusyAlbQnFlZoTJBZY/745MG6aeli6plpR\n"
    "r93G6qVN5VLoXAkvqKslLZlj6wXy70/e0GC0oMFzqSP0AY74icANk8dUFB2Q8usS\n"
    "UseRafNBcYfqACzF/Wa+Fu/upBGwtl7wDLYZdCm3KNjZZZstvVB5DWGnqNX9HkTl\n"
    "U9NBMS/7yph3XYU3mJqUZxryb8pHLVHazarNRppx1aoNroi+5/t3Fx/gEa6a5PoP\n"
    "ouH35DbykmzvVE67GUGpAfZZtEFE1e0E/6IB84PE00llvy3pAgMBAAGjUDBOMB0G\n"
    "A1UdDgQWBBTTi/q1F2iabqlS7yEoX1rbOsz5GDAfBgNVHSMEGDAWgBTTi/q1F2ia\n"
    "bqlS7yEoX1rbOsz5GDAMBgNVHRMEBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQAL\n"
    "aqJ2FgcKLBBHJ8VeNSuGV2cxVYH1JIaHnzL6SlE5q7MYVg+Ofbs2PRlTiWGMazC7\n"
    "q5RKVj9zj0z/8i3ScWrWXFmyp85ZHfuo/DeK6HcbEXJEOfPDvyMPuhVBTzuBIRJb\n"
    "41M27NdIVCdxP6562n6Vp0gbE8kN10q+ksw8YBoLFP0D1da7D5WnSV+nwEIP+F4a\n"
    "3ZX80bNt6tRj9XY0gM68mI60WXrF/qYL+NUz+D3Lw9bgDSXxpSN8JGYBR85BxBvR\n"
    "NNAhsJJ3yoAvbPUQ4m8J/CoVKKgcWymS1pvEHmF47pgzbbjm5bdthlIx+swdiGFa\n"
    "WzdhzTYwVkxBaU+xf/2w\n"
    "-----END CERTIFICATE-----\n";
static esp_mqtt_client_handle_t mqtt_handle = NULL;
static bool mqtt_connected = false;
static uint32_t s_msg_seq = 0;  // 消息序列号，保证id唯一

#define MQTT_RX_PAYLOAD_MAX 1024
static char s_rx_topic[128];
static char s_rx_payload[MQTT_RX_PAYLOAD_MAX + 1];
static int  s_rx_total = 0;
static bool s_rx_drop = false;

/* ---- 回调指针 ---- */
static onenet_property_set_cb_t   s_property_set_cb   = NULL;
static onenet_property_query_cb_t s_property_query_cb = NULL;
static onenet_service_cb_t        s_service_cb        = NULL;


static void generate_msg_id(char *buf, int buf_size)
{
    snprintf(buf, buf_size, "%llu%04x",
             (unsigned long long)(esp_timer_get_time() / 1000),
             (unsigned int)s_msg_seq++);
}

static void extract_msg_id(const char *data, int data_len,
                           char *msg_id, int msg_id_size)
{
    msg_id[0] = '\0';
    const char *end = data + data_len;


    const char *p = NULL;
    for (int i = 0; i <= data_len - 4; i++) {
        if (memcmp(data + i, "\"id\"", 4) == 0) {
            p = data + i;
            break;
        }
    }
    if (!p) return;
    p += 4;

    while (p < end && (*p == ':' || *p == ' ' || *p == '\t')) p++;
    if (p >= end || *p != '"') return;
    p++;

    const char *q = p;
    while (q < end && *q != '"') q++;

    int len = q - p;
    if (len >= msg_id_size) len = msg_id_size - 1;
    memcpy(msg_id, p, len);
    msg_id[len] = '\0';
}

static void handle_complete_message(const char *topic, const char *payload, int payload_len)
{
    ESP_LOGI(TAG, "Received — Topic: %s", topic);
    ESP_LOGI(TAG, "Payload: %.*s", payload_len, payload);

    char msg_id[64] = {0};
    extract_msg_id(payload, payload_len, msg_id, sizeof(msg_id));
    if (!msg_id[0]) {
        ESP_LOGW(TAG, "Ignoring downlink without a valid string id");
        return;
    }

    if (strstr(topic, "/thing/property/set") && !strstr(topic, "_reply")) {
        if (s_property_set_cb) s_property_set_cb(msg_id, payload, payload_len);
    } else if (strstr(topic, "/thing/property/query") && !strstr(topic, "_reply")) {
        if (s_property_query_cb) s_property_query_cb(msg_id);
    } else if (strstr(topic, "/thing/service/")) {
        const char *svc_start = strstr(topic, "/thing/service/");
        if (!svc_start) return;
        svc_start += strlen("/thing/service/");
        const char *svc_end = strstr(svc_start, "/invoke");
        if (svc_end && s_service_cb) {
            char service_id[32] = {0};
            int slen = (int)(svc_end - svc_start);
            if (slen >= (int)sizeof(service_id)) slen = (int)sizeof(service_id) - 1;
            memcpy(service_id, svc_start, slen);
            s_service_cb(msg_id, service_id, payload, payload_len);
        }
    }
}




static void reconnect_timer_cb(void *arg)
{
    if (mqtt_handle == NULL) return;

    if (dev_token_generate(s_token, SIG_METHOD_SHA256, 2524608000,
                           ONENET_PRODUCT_ID, ONENET_DEVICE_NAME,
                           ONENET_DEVICE_KEY) != 0) {
        ESP_LOGE(TAG, "Token refresh failed; reconnect skipped");
        return;
    }

    ESP_LOGI(TAG, "Reconnecting (attempt %d, backoff %ds)...",
             s_reconnect_count, s_backoff_sec);
    esp_err_t err = esp_mqtt_client_reconnect(mqtt_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect failed (%s), will retry", esp_err_to_name(err));
    }
}



static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT connected to OneNet (OneJson 物模型模式)");
        mqtt_connected = true;
        s_backoff_sec = 5;
        s_reconnect_count = 0;
        if (s_reconnect_timer && esp_timer_is_active(s_reconnect_timer)) {
            esp_timer_stop(s_reconnect_timer);
        }

        // 订阅物模型下行Topic
        {
            char topic[128];

            snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/set",
                     ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
            esp_mqtt_client_subscribe(mqtt_handle, topic, 1);
            ESP_LOGI(TAG, "Subscribed: %s", topic);

            snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/query",
                     ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
            esp_mqtt_client_subscribe(mqtt_handle, topic, 1);
            ESP_LOGI(TAG, "Subscribed: %s", topic);

            snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/service/+/invoke",
                     ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);
            esp_mqtt_client_subscribe(mqtt_handle, topic, 1);
            ESP_LOGI(TAG, "Subscribed: %s", topic);
        }
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT disconnected from OneNet");
        mqtt_connected = false;
        s_reconnect_count++;
        ESP_LOGW(TAG, "Will reconnect in %ds (attempt %d, exponential backoff)",
                 s_backoff_sec, s_reconnect_count);
        if (s_reconnect_timer) {
            if (esp_timer_is_active(s_reconnect_timer)) {
                esp_timer_stop(s_reconnect_timer);
            }
            esp_err_t timer_err = esp_timer_start_once(
                s_reconnect_timer, (uint64_t)s_backoff_sec * 1000000);
            if (timer_err != ESP_OK) {
                ESP_LOGE(TAG, "Reconnect timer start failed: %s",
                         esp_err_to_name(timer_err));
            }
        }
        s_backoff_sec = (s_backoff_sec * 2 > BACKOFF_MAX_SEC)
                            ? BACKOFF_MAX_SEC
                            : s_backoff_sec * 2;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "Subscribe success, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "Unsubscribe success, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGD(TAG, "Publish success, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA: {
        if (event->current_data_offset == 0) {
            memset(s_rx_topic, 0, sizeof(s_rx_topic));
            s_rx_total = event->total_data_len;
            s_rx_drop = s_rx_total <= 0 || s_rx_total > MQTT_RX_PAYLOAD_MAX;

            if (event->topic && event->topic_len > 0) {
                int tlen = event->topic_len < (int)sizeof(s_rx_topic) - 1
                               ? event->topic_len : (int)sizeof(s_rx_topic) - 1;
                memcpy(s_rx_topic, event->topic, tlen);
            }
            if (s_rx_drop) {
                ESP_LOGW(TAG, "Dropping oversized MQTT payload (%d bytes, max=%d)",
                         s_rx_total, MQTT_RX_PAYLOAD_MAX);
            }
        }

        if (!s_rx_drop && event->current_data_offset >= 0 && event->data_len >= 0 &&
            event->current_data_offset + event->data_len <= s_rx_total) {
            memcpy(s_rx_payload + event->current_data_offset, event->data, event->data_len);
            int received_end = event->current_data_offset + event->data_len;
            if (received_end == s_rx_total) {
                s_rx_payload[s_rx_total] = '\0';
                handle_complete_message(s_rx_topic, s_rx_payload, s_rx_total);
            }
        } else if (!s_rx_drop) {
            ESP_LOGW(TAG, "Invalid MQTT fragment offset=%d len=%d total=%d",
                     event->current_data_offset, event->data_len, s_rx_total);
            s_rx_drop = true;
        }
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT error occurred");
        break;

    default:
        ESP_LOGD(TAG, "Other MQTT event, id=%ld", event_id);
        break;
    }
}

static esp_err_t publish_checked(const char *topic, const char *payload)
{
    int msg_id = esp_mqtt_client_publish(mqtt_handle, topic, payload, 0, 1, 0);
    if (msg_id < 0) {
        ESP_LOGE(TAG, "Publish enqueue failed: topic=%s", topic);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/*============================================================================
 * 公共 API
 *============================================================================*/

esp_err_t onenet_start(void)
{
    if (mqtt_handle != NULL) return ESP_OK;

    /* 生成 OneNet 设备鉴权 token — 使用设备密钥 */
    if (dev_token_generate(s_token, SIG_METHOD_SHA256, 2524608000,
                           ONENET_PRODUCT_ID, ONENET_DEVICE_NAME,
                           ONENET_DEVICE_KEY) != 0) {
        ESP_LOGE(TAG, "OneNET token generation failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Username=%s, ClientID=%s", ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    /* 创建重连定时器（仅首次） */
    if (s_reconnect_timer == NULL) {
        esp_timer_create_args_t timer_args = {
            .callback = reconnect_timer_cb,
            .arg       = NULL,
            .name      = "mqtt_reconn",
        };
        esp_err_t timer_err = esp_timer_create(&timer_args, &s_reconnect_timer);
        if (timer_err != ESP_OK) return timer_err;
    }

    esp_mqtt_client_config_t mqtt_config = {
        .broker = {
            .address = {
                .hostname  = "studio-mqtts.heclouds.com",
                .port      = 8883,
                .transport = MQTT_TRANSPORT_OVER_SSL,
            },
            .verification = {
                .certificate       = ONENET_CA_CERT,
                .skip_cert_common_name_check = false,
            },
        },
        .credentials = {
            .username    = ONENET_PRODUCT_ID,
            .client_id   = ONENET_DEVICE_NAME,
            .authentication = {
                .password = s_token,
            },
        },
        .network = {
            .timeout_ms               = 15000,
            .reconnect_timeout_ms     = 1000,
            .disable_auto_reconnect   = true,
        },
        .session = {
            .disable_clean_session = false,     /* OneNet 要求 clean session */
            .keepalive             = 120,       /* MQTT keepalive 秒 */
            .protocol_ver          = MQTT_PROTOCOL_V_3_1_1,
        },
        .task = {
            .stack_size = 4096,
            .priority   = 5,
        },
    };

    mqtt_handle = esp_mqtt_client_init(&mqtt_config);
    if (mqtt_handle == NULL) {
        ESP_LOGE(TAG, "MQTT client initialization failed");
        return ESP_FAIL;
    }

    esp_err_t err = esp_mqtt_client_register_event(mqtt_handle, MQTT_EVENT_ANY,
                                                    mqtt_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register MQTT event handler: %s",
                 esp_err_to_name(err));
        esp_mqtt_client_destroy(mqtt_handle);
        mqtt_handle = NULL;
        return err;
    }

    err = esp_mqtt_client_start(mqtt_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client: %s", esp_err_to_name(err));
        esp_mqtt_client_destroy(mqtt_handle);
        mqtt_handle = NULL;
        return err;
    }

    ESP_LOGI(TAG, "OneNet MQTT started (OneJson 物模型)");
    return ESP_OK;
}

/* ---- 属性上报 ---- */

esp_err_t onenet_report_properties(const char *params_json)
{
    if (mqtt_handle == NULL || !mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/post",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    char msg_id[32];
    generate_msg_id(msg_id, sizeof(msg_id));

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"version\":\"1.0\",\"params\":{%s}}",
             msg_id, params_json);

    esp_err_t err = publish_checked(topic, payload);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Property report → %s", payload);
    return ESP_OK;
}

esp_err_t onenet_report_property(const char *name, float value)
{
    char params[128];
    snprintf(params, sizeof(params), "\"%s\":{\"value\":%.1f}", name, value);
    return onenet_report_properties(params);
}

esp_err_t onenet_report_property_int(const char *name, int value)
{
    char params[128];
    snprintf(params, sizeof(params), "\"%s\":{\"value\":%d}", name, value);
    return onenet_report_properties(params);
}

/* ---- 事件上报 ---- */

esp_err_t onenet_report_event(const char *event_id, const char *params_json)
{
    if (mqtt_handle == NULL || !mqtt_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/event/post",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    char msg_id[32];
    generate_msg_id(msg_id, sizeof(msg_id));

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"version\":\"1.0\",\"params\":{\"%s\":{\"value\":%s}}}",
             msg_id, event_id, params_json);

    esp_err_t err = publish_checked(topic, payload);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Event report   → %s", payload);
    return ESP_OK;
}

/* ---- 连接状态 ---- */

bool onenet_is_connected(void)
{
    return mqtt_connected;
}

/* ---- 注册回调 ---- */

void onenet_register_property_set_callback(onenet_property_set_cb_t cb)
{
    s_property_set_cb = cb;
}

void onenet_register_property_query_callback(onenet_property_query_cb_t cb)
{
    s_property_query_cb = cb;
}

void onenet_register_service_callback(onenet_service_cb_t cb)
{
    s_service_cb = cb;
}



esp_err_t onenet_property_set_reply(const char *msg_id, int code, const char *msg)
{
    if (mqtt_handle == NULL || !mqtt_connected || msg_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/set_reply",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
             msg_id, code, msg ? msg : "success");

    esp_err_t err = publish_checked(topic, payload);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Set reply     → %s", payload);
    return ESP_OK;
}

esp_err_t onenet_property_query_reply(const char *msg_id,
                                       const char *params_json)
{
    if (mqtt_handle == NULL || !mqtt_connected || msg_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/query_reply",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME);

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\",\"params\":{%s}}",
             msg_id, params_json ? params_json : "");

    esp_err_t err = publish_checked(topic, payload);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Query reply   → %s", payload);
    return ESP_OK;
}

esp_err_t onenet_service_reply(const char *service_id, const char *msg_id,
                                int code, const char *msg)
{
    if (mqtt_handle == NULL || !mqtt_connected ||
        service_id == NULL || msg_id == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    char topic[128];
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/service/%s/invoke_reply",
             ONENET_PRODUCT_ID, ONENET_DEVICE_NAME, service_id);

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%s\",\"code\":%d,\"msg\":\"%s\"}",
             msg_id, code, msg ? msg : "success");

    esp_err_t err = publish_checked(topic, payload);
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "Service reply → %s", payload);
    return ESP_OK;
}
