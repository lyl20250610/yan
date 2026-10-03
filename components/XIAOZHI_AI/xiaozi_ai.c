
#include "xiaozi_ai.h"
#include "voice.h"
#include "wifi.h"

#include "esp_websocket_client.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"         
#include "nvs.h"
#include "cJSON.h"
#include "opus.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"

static const char *TAG = "XIAOZHI_AI";

#define AUDIO_SAMPLE_HZ       16000
#define AUDIO_CHANNELS        1
#define OPUS_FRAME_MS         60
#define OPUS_FRAME_SAMPLES    ((AUDIO_SAMPLE_HZ) * (OPUS_FRAME_MS) / 1000)  // 960
#define OPUS_MAX_BYTES        256

#define DEFAULT_SERVER_URL    "wss://api.tenclass.net/xiaozhi/v1/"
#define OTA_URL               "https://api.tenclass.net/xiaozhi/ota/"
#define APP_VERSION            "1.0.0"
#define BOARD_TYPE             "ESP32-S3-AIoT"


#define NVS_NAMESPACE          "xiaozhi"
#define NVS_KEY_CLIENT_ID      "client_id"
#define NVS_KEY_WS_TOKEN       "ws_token"
#define NVS_KEY_ACT_CODE       "act_code"


static char g_client_id[37] = {0};


static char g_ota_token[256] = {0};
static char g_ota_ws_url[256] = {0};
static char g_activation_code[16] = {0};   
static bool g_tts_muted = false;           

static esp_err_t client_id_init(void)
{
    nvs_handle_t nvs;
    size_t len = sizeof(g_client_id);
    esp_err_t err;

    err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open fail: %s", esp_err_to_name(err));
        uint8_t mac[6];
        esp_efuse_mac_get_default(mac);
        snprintf(g_client_id, sizeof(g_client_id),
                 "%02X%02X%02X%02X%02X%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        return ESP_OK;
    }

    err = nvs_get_str(nvs, NVS_KEY_CLIENT_ID, g_client_id, &len);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Client-ID (from NVS): %s", g_client_id);
        nvs_close(nvs);
        return ESP_OK;
    }

    uint32_t r[4];
    for (int i = 0; i < 4; i++) {
        r[i] = esp_random();
    }
    snprintf(g_client_id, sizeof(g_client_id),
             "%08X%08X%08X%08X",
             (unsigned int)r[0], (unsigned int)r[1], (unsigned int)r[2], (unsigned int)r[3]);
    g_client_id[8]  = '-';  
    g_client_id[13] = '-';
    g_client_id[18] = '-';
    g_client_id[23] = '-';
    snprintf(g_client_id, sizeof(g_client_id),
             "%08X-%04X-%04X-%04X-%04X%08X",
             (unsigned int)r[0],
             (unsigned int)(uint16_t)(r[1] >> 16), (unsigned int)(uint16_t)(r[1] & 0xFFFF),
             (unsigned int)(uint16_t)(r[2] >> 16), (unsigned int)(uint16_t)(r[2] & 0xFFFF),
             (unsigned int)r[3]);

    err = nvs_set_str(nvs, NVS_KEY_CLIENT_ID, g_client_id);
    if (err == ESP_OK) {
        nvs_commit(nvs);
        ESP_LOGI(TAG, "Client-ID (new): %s", g_client_id);
    }
    nvs_close(nvs);
    return ESP_OK;
}

#define BIT_CONNECTED          BIT0
#define BIT_LISTENING          BIT1
#define BIT_MCP_DONE           BIT2   // MCP 握手完成，可以安全收发数据

static TaskHandle_t      g_xz_ws_task  = NULL;

typedef struct {
    uint8_t data[256];   // Opus 编码数据（最大 256 字节）
    int     len;
} audio_frame_t;

#define DEFER_ABORT_AND_WAKE  1
#define DEFER_MCP_RESPONSE    2
#define DEFER_AUTO_LISTEN     3
#define DEFER_SEND_HELLO      4
#define DEFER_RECONNECT       5
#define DEFER_SEND_TEXT       6

typedef struct {
    int  action;
    char text[128];    
    int  param;        
} defer_msg_t;

/* 前向声明 */
static void send_hello(void);
static void send_mcp_response(int mcp_id, const char *session_id);

static QueueHandle_t g_audio_queue = NULL;      // 接收音频: WS → 解码器 → 扬声器
static TaskHandle_t  g_audio_task  = NULL;
static QueueHandle_t g_defer_queue = NULL;
static TaskHandle_t  g_defer_task  = NULL;


static xiaozhi_ai_config_t         g_cfg;
static bool                        g_inited = false;

static esp_websocket_client_handle_t g_ws = NULL;
static EventGroupHandle_t          g_evtgrp = NULL;
static TaskHandle_t                g_cap_task = NULL;

static OpusEncoder                *g_enc = NULL;
static OpusDecoder                *g_dec = NULL;

static volatile bool               g_am_listening = false;

/* ---- 主动重连（指数退避）---- */
static int          g_reconnect_attempts = 0;
static bool         g_reconnect_pending  = false;
#define RECONNECT_BASE_DELAY_MS   2000    /* 首次重连等待 2s */
#define RECONNECT_MAX_DELAY_MS   60000    /* 最大间隔 60s */



static esp_err_t ota_register(void)
{
    // 获取 MAC 地址
    uint8_t mac[6];
    char mac_str[18];
    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        ESP_LOGE(TAG, "OTA: Failed to get MAC");
        return ESP_FAIL;
    }
    snprintf(mac_str, sizeof(mac_str),
             "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // 构造请求体 — 匹配官方 xiaozhi-esp32 协议格式
    cJSON *body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "version", APP_VERSION);
    cJSON_AddStringToObject(body, "board", BOARD_TYPE);
    cJSON_AddStringToObject(body, "mac_address", mac_str);
    cJSON_AddNumberToObject(body, "flash_size", 8 * 1024 * 1024);  // 8MB
    cJSON_AddStringToObject(body, "chip_model_name", "esp32s3");
    char *body_str = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!body_str) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "OTA: Requesting activation...");
    ESP_LOGI(TAG, "OTA:   MAC=%s  ClientID=%s", mac_str, g_client_id);

    // 先尝试从 NVS 读取缓存的 OTA 结果
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len;
            len = sizeof(g_ota_ws_url);
            if (nvs_get_str(nvs, "ota_ws_url", g_ota_ws_url, &len) == ESP_OK && g_ota_ws_url[0]) {
                ESP_LOGI(TAG, "OTA: Using cached WS URL: %s", g_ota_ws_url);
            }
            len = sizeof(g_ota_token);
            if (nvs_get_str(nvs, "ota_token", g_ota_token, &len) == ESP_OK && g_ota_token[0]) {
                ESP_LOGI(TAG, "OTA: Using cached token");
            }
            nvs_close(nvs);
        }
    }

    // HTTP 配置 — 增加超时，下面有重试逻辑
    esp_http_client_config_t http_cfg = {
        .url = OTA_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,  // 15秒超时，适应慢速网络和DNS重试
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t http = esp_http_client_init(&http_cfg);
    if (!http) {
        free(body_str);
        return ESP_ERR_NO_MEM;
    }

    // 请求头（只需设置一次，重试时复用）
    esp_http_client_set_header(http, "Content-Type", "application/json");
    esp_http_client_set_header(http, "Device-Id", mac_str);
    esp_http_client_set_header(http, "Client-Id", g_client_id);
    esp_http_client_set_header(http, "Activation-Version", "1");
    esp_http_client_set_header(http, "User-Agent", BOARD_TYPE "/" APP_VERSION);
    esp_http_client_set_post_field(http, body_str, strlen(body_str));

    // 重试循环：最多 2 次，第 1 次失败等 2 秒再试
    #define OTA_MAX_ATTEMPTS 2
    bool ota_ok = false;
    for (int ota_attempt = 0; ota_attempt < OTA_MAX_ATTEMPTS && !ota_ok; ota_attempt++) {
        if (ota_attempt > 0) {
            ESP_LOGW(TAG, "OTA: Retrying after 2s delay (attempt %d/%d)...",
                     ota_attempt + 1, OTA_MAX_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
        ESP_LOGI(TAG, "OTA: Attempt %d/%d...", ota_attempt + 1, OTA_MAX_ATTEMPTS);

        esp_err_t err = esp_http_client_open(http, strlen(body_str));
        if (err == ESP_OK) {
            int wrote = esp_http_client_write(http, body_str, strlen(body_str));
            ESP_LOGI(TAG, "OTA: wrote %d bytes", wrote);
            int content_len = esp_http_client_fetch_headers(http);
            int status = esp_http_client_get_status_code(http);
            ESP_LOGI(TAG, "OTA: HTTP %d, content_len=%d", status, content_len);

            if (status == 200) {
                // 循环读取，ESP-IDF HTTP client 内部有 buffer，单次 read 最多返回 buffer 大小
                char *resp = NULL;
                int total_read = 0;
                int buf_cap = (content_len > 0) ? (content_len + 1) : 2048;
                resp = malloc(buf_cap);
                if (resp) {
                    int chunk;
                    while ((chunk = esp_http_client_read(http, resp + total_read,
                                                          buf_cap - total_read - 1)) > 0) {
                        total_read += chunk;
                        // 如果 buffer 不够，动态扩展
                        if (total_read + 512 >= buf_cap) {
                            buf_cap *= 2;
                            char *new_resp = realloc(resp, buf_cap);
                            if (!new_resp) break;
                            resp = new_resp;
                        }
                    }
                    resp[total_read] = '\0';
                    ESP_LOGI(TAG, "OTA: read=%d bytes, content_len=%d", total_read, content_len);

                    if (total_read > 0) {
                        // 打印前 500 字节
                        ESP_LOGI(TAG, "OTA: Response: %.*s",
                                 (total_read < 500) ? total_read : 500, resp);

                        // 如果不是以 { 开头，hex dump 前 64 字节排查
                        if (resp[0] != '{') {
                            ESP_LOGW(TAG, "OTA: Response doesn't start with '{', hex dump:");
                            char hex[128 + 1] = {0};
                            for (int i = 0; i < total_read && i < 64; i++) {
                                snprintf(hex + i * 2, 3, "%02X", (unsigned char)resp[i]);
                            }
                            ESP_LOGW(TAG, "  %s", hex);
                        }

                        cJSON *r = cJSON_Parse(resp);
                        if (r) {
                            // 先提取 websocket 凭据（后续判断激活是否阻塞）
                            bool has_ws = false;
                            cJSON *ws = cJSON_GetObjectItem(r, "websocket");
                            if (ws) {
                                cJSON *ws_url = cJSON_GetObjectItem(ws, "url");
                                if (ws_url && cJSON_IsString(ws_url) && ws_url->valuestring) {
                                    strncpy(g_ota_ws_url, ws_url->valuestring, sizeof(g_ota_ws_url) - 1);
                                    g_ota_ws_url[sizeof(g_ota_ws_url) - 1] = '\0';
                                    has_ws = true;
                                    ESP_LOGI(TAG, "OTA: Server WS URL: %s", g_ota_ws_url);
                                }
                                cJSON *ws_tok = cJSON_GetObjectItem(ws, "token");
                                if (ws_tok && cJSON_IsString(ws_tok) && ws_tok->valuestring) {
                                    strncpy(g_ota_token, ws_tok->valuestring, sizeof(g_ota_token) - 1);
                                    g_ota_token[sizeof(g_ota_token) - 1] = '\0';
                                    ESP_LOGI(TAG, "OTA: Server supplied a WS token");
                                }
                            }

                            // 缓存 OTA 结果到 NVS，下次启动跳过 HTTP 请求
                            if (g_ota_token[0] || g_ota_ws_url[0]) {
                                nvs_handle_t nvs;
                                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                                    if (g_ota_ws_url[0]) nvs_set_str(nvs, "ota_ws_url", g_ota_ws_url);
                                    if (g_ota_token[0]) nvs_set_str(nvs, "ota_token", g_ota_token);
                                    if (g_activation_code[0]) nvs_set_str(nvs, NVS_KEY_ACT_CODE, g_activation_code);
                                    nvs_commit(nvs);
                                    nvs_close(nvs);
                                    ESP_LOGI(TAG, "OTA: Saved to NVS cache");
                                }
                            }

                            // 激活提示：只在没有 WebSocket 凭据时才弹警告
                            cJSON *act = cJSON_GetObjectItem(r, "activation");
                            if (act) {
                                cJSON *code = cJSON_GetObjectItem(act, "code");
                                if (code && cJSON_IsString(code) && code->valuestring) {
                                    // 保存激活码，供外部查询
                                    strncpy(g_activation_code, code->valuestring,
                                            sizeof(g_activation_code) - 1);
                                    if (!has_ws) {
                                        // 无 WS 凭据 = 真正需要激活才能连接
                                        ESP_LOGW(TAG, "============================================");
                                        ESP_LOGW(TAG, "  ⭐ 设备需要激活！");
                                        ESP_LOGW(TAG, "  激活码: %s", code->valuestring);
                                        ESP_LOGW(TAG, "  请打开浏览器访问: https://xiaozhi.me");
                                        ESP_LOGW(TAG, "  登录后输入以上激活码完成设备绑定");
                                        ESP_LOGW(TAG, "============================================");
                                    } else {
                                        // 服务器已给 WS 凭据，能正常连接，静默记录即可
                                        ESP_LOGI(TAG, "OTA: Activation code: %s (已有WS凭据，无需弹窗)",
                                                 code->valuestring);
                                    }
                                }
                            }

                            cJSON_Delete(r);
                            ota_ok = true;  // 标记成功，退出重试循环
                        } else {
                            ESP_LOGW(TAG, "OTA: JSON parse failed — raw: %s",
                                     total_read > 200 ? "(too long)" : resp);
                        }
                    } else {
                        ESP_LOGW(TAG, "OTA: Zero-length body (status=%d)", status);
                    }
                    free(resp);
                } else {
                    ESP_LOGE(TAG, "OTA: malloc failed");
                }
            } else {
                ESP_LOGW(TAG, "OTA: HTTP %d (err=%s)", status, esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "OTA: open failed (attempt %d): %s",
                     ota_attempt + 1, esp_err_to_name(err));
        }

        // 失败时关闭连接以便重试（成功时在循环条件中自然退出）
        if (!ota_ok) {
            esp_http_client_close(http);
        }
    }

    if (!ota_ok) {
        ESP_LOGE(TAG, "OTA: All %d attempts failed — AI features may be limited", OTA_MAX_ATTEMPTS);
    }
    #undef OTA_MAX_ATTEMPTS
    esp_http_client_cleanup(http);
    free(body_str);
    return ESP_OK;
}



static inline void notify(xiaozhi_event_t ev, const char *data)
{
    if (g_cfg.event_cb) g_cfg.event_cb(ev, data, g_cfg.user_data);
}

/* ---- 发送 Binary (v1: raw Opus, no header) ---- */
static esp_err_t ws_bin_send(const uint8_t *opus, uint16_t len)
{
    if (!g_ws) return ESP_FAIL;
    if (!(xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED)) return ESP_FAIL;
    int r = esp_websocket_client_send_bin(g_ws, (const char *)opus,
                                           len, pdMS_TO_TICKS(200));
    return (r >= 0) ? ESP_OK : ESP_FAIL;
}

/* ---- 发送 JSON ---- */
static esp_err_t ws_json_send(cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    if (!s) return ESP_ERR_NO_MEM;
    int r = -1;
    if (g_ws && (xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED)) {
        r = esp_websocket_client_send_text(g_ws, s, strlen(s), pdMS_TO_TICKS(500));
    }
    free(s);
    return (r >= 0) ? ESP_OK : ESP_FAIL;
}

/* ---- 前向声明 ---- */
static void on_json(const char *data, int len);

/* ---- WebSocket hello ---- */
static void send_hello(void)
{
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "hello");
    cJSON_AddNumberToObject(h, "version", 1);
    cJSON_AddStringToObject(h, "transport", "websocket");

    // features (matches official 78/xiaozhi-esp32)
    cJSON *feat = cJSON_CreateObject();
    cJSON_AddBoolToObject(feat, "mcp", true);
    cJSON_AddBoolToObject(feat, "aec", true);
    cJSON_AddItemToObject(h, "features", feat);

    // audio_params
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "format", "opus");
    cJSON_AddNumberToObject(a, "sample_rate", AUDIO_SAMPLE_HZ);
    cJSON_AddNumberToObject(a, "channels", AUDIO_CHANNELS);
    cJSON_AddNumberToObject(a, "frame_duration", OPUS_FRAME_MS);
    cJSON_AddItemToObject(h, "audio_params", a);

    // 设备标识 — 服务器需要 device_id + client_id 来识别设备
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK) {
        char device_id[18];
        snprintf(device_id, sizeof(device_id),
                 "%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        cJSON_AddStringToObject(h, "device_id", device_id);
    }
    cJSON_AddStringToObject(h, "client_id", g_client_id);

    // iot 设备描述 — 描述设备的输入输出能力
    cJSON *iot = cJSON_CreateObject();
    cJSON *iot_desc = cJSON_CreateArray();

    cJSON *mic = cJSON_CreateObject();
    cJSON_AddStringToObject(mic, "name", "microphone");
    cJSON_AddStringToObject(mic, "description", "麦克风");
    cJSON_AddItemToArray(iot_desc, mic);

    cJSON *spk = cJSON_CreateObject();
    cJSON_AddStringToObject(spk, "name", "speaker");
    cJSON_AddStringToObject(spk, "description", "扬声器");
    cJSON_AddItemToArray(iot_desc, spk);

    cJSON_AddItemToObject(iot, "descriptors", iot_desc);
    cJSON_AddItemToObject(h, "iot", iot);

    char *hello_str = cJSON_PrintUnformatted(h);
    ESP_LOGI(TAG, "Sending hello: %s", hello_str);
    int r = esp_websocket_client_send_text(g_ws, hello_str, strlen(hello_str), pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Hello send result: %d", r);
    free(hello_str);
    cJSON_Delete(h);
}

static void send_listen(const char *state, const char *mode)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "listen");
    cJSON_AddStringToObject(m, "state", state);
    if (mode) cJSON_AddStringToObject(m, "mode", mode);
    ws_json_send(m);
    cJSON_Delete(m);
}

static void send_wake(const char *text)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "listen");
    cJSON_AddStringToObject(m, "state", "detect");
    if (text) cJSON_AddStringToObject(m, "text", text);
    ws_json_send(m);
    cJSON_Delete(m);
}

static void send_abort_msg(void)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "type", "abort");
    ws_json_send(m);
    cJSON_Delete(m);
}

// 发送文字消息 — 依次尝试多种协议格式，看服务器接受哪个
#define TEXT_PROBE_FORMAT  2  // 1=stt逆向  2=listen/detect  3=chat类型  4=MCP tools/call
static void send_text_message(const char *text)
{
    if (!text || !text[0]) return;
    cJSON *m = cJSON_CreateObject();

#if TEXT_PROBE_FORMAT == 1
    // 格式1: 逆向 stt（已测试 → 服务器无响应）
    cJSON_AddStringToObject(m, "type", "stt");
    cJSON_AddStringToObject(m, "text", text);
    ESP_LOGI(TAG, "Sending text [stt]: %s", text);

#elif TEXT_PROBE_FORMAT == 2
    // 格式2: 唤醒词检测路径（服务器已处理此格式，可能接受全文）
    // 先 abort 再发 detect + text
    cJSON *abort = cJSON_CreateObject();
    cJSON_AddStringToObject(abort, "type", "abort");
    ws_json_send(abort);
    cJSON_Delete(abort);
    vTaskDelay(pdMS_TO_TICKS(200));

    cJSON_AddStringToObject(m, "type", "listen");
    cJSON_AddStringToObject(m, "state", "detect");
    cJSON_AddStringToObject(m, "text", text);
    ESP_LOGI(TAG, "Sending text [listen/detect]: %s", text);

#elif TEXT_PROBE_FORMAT == 3
    // 格式3: 通用 chat 类型
    cJSON_AddStringToObject(m, "type", "chat");
    cJSON_AddStringToObject(m, "text", text);
    ESP_LOGI(TAG, "Sending text [chat]: %s", text);

#elif TEXT_PROBE_FORMAT == 4
    // 格式4: MCP tools/call（需要先有 session_id）
    cJSON_AddStringToObject(m, "type", "mcp");
    // session_id 由 ws_json_send 外部处理
    {
        cJSON *payload = cJSON_CreateObject();
        cJSON_AddStringToObject(payload, "jsonrpc", "2.0");
        cJSON_AddStringToObject(payload, "method", "tools/call");
        cJSON_AddNumberToObject(payload, "id", 99);
        cJSON *params = cJSON_CreateObject();
        cJSON_AddStringToObject(params, "name", "chat");
        cJSON *args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "text", text);
        cJSON_AddItemToObject(params, "arguments", args);
        cJSON_AddItemToObject(payload, "params", params);
        cJSON_AddItemToObject(m, "payload", payload);
    }
    ESP_LOGI(TAG, "Sending text [mcp tools/call]: %s", text);
#endif

    ws_json_send(m);
    cJSON_Delete(m);
}



static esp_err_t opus_init(void)
{
    int e;

    g_enc = opus_encoder_create(AUDIO_SAMPLE_HZ, AUDIO_CHANNELS,
                                 OPUS_APPLICATION_VOIP, &e);
    if (e != OPUS_OK) { ESP_LOGE(TAG, "enc fail %d", e); return ESP_FAIL; }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(32000));
    opus_encoder_ctl(g_enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    g_dec = opus_decoder_create(AUDIO_SAMPLE_HZ, AUDIO_CHANNELS, &e);
    if (e != OPUS_OK) { ESP_LOGE(TAG, "dec fail %d", e); return ESP_FAIL; }

    ESP_LOGI(TAG, "Opus codec ok");
    return ESP_OK;
}

static void opus_deinit(void)
{
    if (g_enc) { opus_encoder_destroy(g_enc); g_enc = NULL; }
    if (g_dec) { opus_decoder_destroy(g_dec); g_dec = NULL; }
}



/* ---- 音频播放任务（解码 Opus → I2S 输出，避免阻塞 WS 任务）---- */
static void audio_player_task(void *arg)
{
    audio_frame_t frame;
    int16_t *pcm = heap_caps_malloc(OPUS_FRAME_SAMPLES * 2 * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "audio_player: pcm alloc failed");
        vTaskDelete(NULL);
        return;
    }
    while (1) {
        if (xQueueReceive(g_audio_queue, &frame, portMAX_DELAY) == pdTRUE) {
            int n = opus_decode(g_dec, frame.data, frame.len,
                               pcm, OPUS_FRAME_SAMPLES * 2, 0);
            if (n > 0) {
                if (!g_tts_muted) voice_play_audio(pcm, n * sizeof(int16_t));
            } else if (n < 0) {
                ESP_LOGW(TAG, "opus decode err: %d", n);
            }
        }
    }
}

/* ---- 音频发送（内部锁自动串行化，无需挂起 WS 任务）---- */
static esp_err_t safe_ws_bin_send(const uint8_t *data, uint16_t len)
{
    if (!g_ws) return ESP_ERR_INVALID_STATE;
    return ws_bin_send(data, len);
}

/* ---- MCP 响应（snprintf 构建，避免 cJSON 递归序列化在 WS 任务栈上溢出）---- */
static void send_mcp_response(int mcp_id, const char *session_id)
{
    if (!g_ws) return;
    if (g_evtgrp && !(xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED)) return;

    char buf[512];
    int len;

    if (session_id && session_id[0]) {
        len = snprintf(buf, sizeof(buf),
            "{\"type\":\"mcp\",\"session_id\":\"%s\","
            "\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"result\":{\"protocolVersion\":\"2024-11-05\","
            "\"capabilities\":{},"
            "\"serverInfo\":{\"name\":\"esp32-s3-aiot\","
            "\"version\":\"%s\"}}}}",
            session_id, mcp_id, APP_VERSION);
    } else {
        len = snprintf(buf, sizeof(buf),
            "{\"type\":\"mcp\","
            "\"payload\":{\"jsonrpc\":\"2.0\",\"id\":%d,"
            "\"result\":{\"protocolVersion\":\"2024-11-05\","
            "\"capabilities\":{},"
            "\"serverInfo\":{\"name\":\"esp32-s3-aiot\","
            "\"version\":\"%s\"}}}}",
            mcp_id, APP_VERSION);
    }

    if (len > 0 && len < (int)sizeof(buf)) {
        esp_websocket_client_send_text(g_ws, buf, len, pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "MCP response sent (id=%d)", mcp_id);
}

/* ---- 延迟发送任务（队列驱动，安全跨任务发送 WS 消息）---- */
static void sender_task(void *arg)
{
    defer_msg_t msg;
    while (1) {
        if (xQueueReceive(g_defer_queue, &msg, pdMS_TO_TICKS(30000)) == pdTRUE) {

            switch (msg.action) {

            case DEFER_SEND_HELLO: {
                /* hello 在独立任务中发送，不在 WS CONNECTED 回调中操作 TLS */
                if (g_ws) {
                    send_hello();
                    ESP_LOGI(TAG, "Hello sent (deferred)");
                }
                break;
            }

            case DEFER_AUTO_LISTEN: {
                /* 连接后自动开始聆听 */
                if (g_ws) {
                    send_listen("start", "auto");
                }
                g_am_listening = true;
                xEventGroupSetBits(g_evtgrp, BIT_LISTENING);
                notify(XIAOZHI_EVENT_LISTENING, NULL);
                ESP_LOGI(TAG, "Auto-listen started after hello (deferred)");
                break;
            }

            case DEFER_MCP_RESPONSE: {
                /* MCP 响应 — 在独立任务中发送，避免 WS DATA 回调内 TLS 重入 */
                if (g_ws) {
                    send_mcp_response(msg.param, msg.text);
                }
                xEventGroupSetBits(g_evtgrp, BIT_MCP_DONE);
                ESP_LOGI(TAG, "MCP initialize responded (deferred)");
                break;
            }

            case DEFER_ABORT_AND_WAKE: {
                /* 唤醒词检测 — 批量发送 abort + wake + listen */
                const char *wake_text = msg.text[0] ? msg.text : NULL;
                if (g_ws) {
                    send_abort_msg();
                    send_wake(wake_text);
                    vTaskDelay(pdMS_TO_TICKS(200));
                    send_listen("start", "auto");
                }
                g_am_listening = true;
                xEventGroupSetBits(g_evtgrp, BIT_LISTENING);
                notify(XIAOZHI_EVENT_LISTENING, NULL);
                ESP_LOGI(TAG, "Wake: %s", wake_text ? wake_text : "(null)");
                break;
            }

            case DEFER_RECONNECT: {
                /* 主动重连 — 指数退避，避免频繁重试压垮服务器 */
                if (!g_ws) break;

                /* 如果已经连上了，重置计数器，不再重连 */
                if (xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED) {
                    g_reconnect_attempts = 0;
                    g_reconnect_pending = false;
                    ESP_LOGI(TAG, "Reconnect: already connected, reset counter");
                    break;
                }

                g_reconnect_attempts++;
                uint32_t delay_ms = RECONNECT_BASE_DELAY_MS;
                int shifts = g_reconnect_attempts - 1;
                if (shifts > 5) shifts = 5;
                while (shifts-- > 0 && delay_ms < RECONNECT_MAX_DELAY_MS) {
                    delay_ms *= 2;
                }
                if (delay_ms > RECONNECT_MAX_DELAY_MS) delay_ms = RECONNECT_MAX_DELAY_MS;

                ESP_LOGI(TAG, "Reconnect: waiting %d ms (attempt %d)...",
                          (int)delay_ms, g_reconnect_attempts);
                vTaskDelay(pdMS_TO_TICKS(delay_ms));

                /* 等待期间可能已被其他路径连上 */
                if (xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED) {
                    g_reconnect_attempts = 0;
                    g_reconnect_pending = false;
                    ESP_LOGI(TAG, "Reconnect: connected during backoff, cancelled");
                    break;
                }

                /* 停止旧连接，重启 client */
                esp_websocket_client_stop(g_ws);
                vTaskDelay(pdMS_TO_TICKS(1000));

                // start() 成功仅表示客户端任务已启动。提前清 pending，确保随后
                // 的 DISCONNECTED/CLOSED 事件还能安排下一次重试。
                g_reconnect_pending = false;
                esp_err_t ret = esp_websocket_client_start(g_ws);
                if (ret == ESP_OK) {
                    ESP_LOGI(TAG, "Reconnect: client restarted, waiting for hello...");
                } else {
                    ESP_LOGE(TAG, "Reconnect: start failed (%s), will retry",
                             esp_err_to_name(ret));
                    /* 重新入队，等待下一次退避重试 */
                    defer_msg_t next = { .action = DEFER_RECONNECT };
                    g_reconnect_pending = true;
                    if (xQueueSend(g_defer_queue, &next, 0) != pdTRUE) {
                        g_reconnect_pending = false;
                        ESP_LOGE(TAG, "Reconnect queue full");
                    }
                }
                break;
            }

            case DEFER_SEND_TEXT:
                if (g_ws && msg.text[0]) {
                    send_text_message(msg.text);
                    ESP_LOGI(TAG, "Text sent (deferred): %s", msg.text);
                }
                break;

            default:
                ESP_LOGW(TAG, "sender_task: unknown action %d", msg.action);
                break;
            }
        } else {
            /* 每 30 秒报告各任务栈水位（用于诊断栈溢出）*/
            ESP_LOGI(TAG, "Stack HWM: xz_send=%u xz_audio=%u xz_cap=%u",
                     g_defer_task ? (unsigned)uxTaskGetStackHighWaterMark(g_defer_task) : 0,
                     g_audio_task ? (unsigned)uxTaskGetStackHighWaterMark(g_audio_task) : 0,
                     g_cap_task   ? (unsigned)uxTaskGetStackHighWaterMark(g_cap_task)   : 0);
        }
    }
}

static void on_json(const char *data, int len)
{
    cJSON *r = cJSON_ParseWithLength(data, len);
    if (!r) return;

    const char *type = NULL;
    cJSON *t = cJSON_GetObjectItem(r, "type");
    if (t && t->valuestring) type = t->valuestring;
    if (!type) { cJSON_Delete(r); return; }

    if (strcmp(type, "hello") == 0) {
        // 检查是否有错误码
        cJSON *code_obj = cJSON_GetObjectItem(r, "code");
        if (code_obj && cJSON_IsNumber(code_obj) && code_obj->valueint != 0) {
            int err_code = code_obj->valueint;
            const char *msg = "server rejected";
            cJSON *msg_obj = cJSON_GetObjectItem(r, "message");
            if (msg_obj && cJSON_IsString(msg_obj)) msg = msg_obj->valuestring;

            ESP_LOGE(TAG, "Server rejected (code=%d): %s", err_code, msg);

            // 检查是否携带激活码
            cJSON *ac = cJSON_GetObjectItem(r, "activation_code");
            if (ac && cJSON_IsString(ac) && ac->valuestring) {
                strncpy(g_activation_code, ac->valuestring, sizeof(g_activation_code) - 1);
                ESP_LOGW(TAG, "============================================");
                ESP_LOGW(TAG, "  ⭐ 设备需要激活！激活码: %s", ac->valuestring);
                ESP_LOGW(TAG, "  请打开 https://xiaozhi.me 输入激活码");
                ESP_LOGW(TAG, "============================================");
            }
            // 检查 text 字段（服务器有时把激活码放在这）
            cJSON *txt = cJSON_GetObjectItem(r, "text");
            if (txt && cJSON_IsString(txt) && txt->valuestring) {
                ESP_LOGI(TAG, "Server text: %s", txt->valuestring);
            }
            notify(XIAOZHI_EVENT_ERROR, msg);
        } else {
            ESP_LOGI(TAG, "Server hello ok");

            // 提取服务器返回的 session token 并持久化到 NVS
            cJSON *tok = cJSON_GetObjectItem(r, "token");
            if (tok && cJSON_IsString(tok) && tok->valuestring
                && strlen(tok->valuestring) > 0
                && strcmp(tok->valuestring, "test-token") != 0) {
                nvs_handle_t nvs;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_set_str(nvs, NVS_KEY_WS_TOKEN, tok->valuestring);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                    ESP_LOGI(TAG, "WS token saved to NVS");
                }
            }

            xEventGroupSetBits(g_evtgrp, BIT_CONNECTED);
            g_reconnect_attempts = 0;
            g_reconnect_pending  = false;
            if (g_activation_code[0]) {
                g_activation_code[0] = '\0';
                nvs_handle_t nvs;
                if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
                    nvs_erase_key(nvs, NVS_KEY_ACT_CODE);
                    nvs_commit(nvs);
                    nvs_close(nvs);
                }
            }
            notify(XIAOZHI_EVENT_CONNECTED, NULL);

            // 自动模式下，通过延迟队列启动聆听（避免在 WS 回调中发送）
            if (g_cfg.mode == XIAOZHI_MODE_AUTO) {
                vTaskDelay(pdMS_TO_TICKS(200));
                defer_msg_t dmsg = { .action = DEFER_AUTO_LISTEN };
                xQueueSend(g_defer_queue, &dmsg, 0);
            }
        }
    }
    else if (strcmp(type, "stt") == 0) {
        const char *text = NULL;
        cJSON *tx = cJSON_GetObjectItem(r, "text");
        if (tx && tx->valuestring) text = tx->valuestring;
        ESP_LOGI(TAG, "ASR: %s", text ? text : "");
        notify(XIAOZHI_EVENT_ASR_TEXT, text);
    }
    else if (strcmp(type, "tts") == 0) {
        const char *st = NULL;
        cJSON *s = cJSON_GetObjectItem(r, "state");
        if (s && s->valuestring) st = s->valuestring;
        if (st && strcmp(st, "start") == 0) {
            notify(XIAOZHI_EVENT_TTS_START, NULL);
            notify(XIAOZHI_EVENT_SPEAKING, NULL);
        } else if (st && strcmp(st, "sentence_start") == 0) {
            // 每句话开始 — 提取 text 字段作为 AI 回复文字
            cJSON *txt = cJSON_GetObjectItem(r, "text");
            if (txt && cJSON_IsString(txt) && txt->valuestring) {
                ESP_LOGI(TAG, "LLM text: %s", txt->valuestring);
                notify(XIAOZHI_EVENT_LLM_TEXT, txt->valuestring);
            }
        } else if (st && strcmp(st, "stop") == 0) {
            notify(XIAOZHI_EVENT_TTS_STOP, NULL);
            // auto mode: resume listening
            if (g_cfg.mode == XIAOZHI_MODE_AUTO && g_am_listening) {
                g_am_listening = true;
                xEventGroupSetBits(g_evtgrp, BIT_LISTENING);
                notify(XIAOZHI_EVENT_LISTENING, NULL);
            }
        }
    }
    else if (strcmp(type, "llm") == 0) {
        // emotion info; no dedicated event, just log
        cJSON *em = cJSON_GetObjectItem(r, "emotion");
        ESP_LOGI(TAG, "LLM emotion: %s",
                 (em && em->valuestring) ? em->valuestring : "none");
    }
    else if (strcmp(type, "mcp") == 0) {
        // MCP (Model Context Protocol) — 通过队列异步响应，避免在 WS 数据回调上下文内
        // 直接调用 esp_websocket_client_send_text() 导致 TLS context 重入 → 栈损坏
        cJSON *payload = cJSON_GetObjectItem(r, "payload");
        if (payload) {
            cJSON *method = cJSON_GetObjectItem(payload, "method");
            if (method && method->valuestring &&
                strcmp(method->valuestring, "initialize") == 0) {
                cJSON *id_obj = cJSON_GetObjectItem(payload, "id");
                cJSON *sid = cJSON_GetObjectItem(r, "session_id");
                int mcp_id = (id_obj && cJSON_IsNumber(id_obj)) ? id_obj->valueint : 1;
                const char *session = (sid && sid->valuestring) ? sid->valuestring : "";
                ESP_LOGI(TAG, "MCP initialize — queuing deferred response");
                defer_msg_t dmsg = { .action = DEFER_MCP_RESPONSE, .param = mcp_id };
                if (session[0]) strncpy(dmsg.text, session, sizeof(dmsg.text) - 1);
                xQueueSend(g_defer_queue, &dmsg, 0);
            }
        }
    }
    else if (strcmp(type, "listen") == 0) {
        // 服务器下发聆听状态控制 — 遵循服务端指令，不自行决定何时发送音频
        const char *state = NULL;
        cJSON *st = cJSON_GetObjectItem(r, "state");
        if (st && st->valuestring) state = st->valuestring;

        if (state && strcmp(state, "start") == 0) {
            // 服务器要求开始采集音频
            ESP_LOGI(TAG, "Server: listen start");
            if (!g_am_listening) {
                g_am_listening = true;
                xEventGroupSetBits(g_evtgrp, BIT_LISTENING);
                notify(XIAOZHI_EVENT_LISTENING, NULL);
            }
        } else if (state && strcmp(state, "stop") == 0) {
            // 服务器要求停止采集音频
            ESP_LOGI(TAG, "Server: listen stop");
            g_am_listening = false;
            xEventGroupClearBits(g_evtgrp, BIT_LISTENING);
        } else if (state && strcmp(state, "detect") == 0) {
            // 服务器通知检测到唤醒词
            const char *text = NULL;
            cJSON *tx = cJSON_GetObjectItem(r, "text");
            if (tx && tx->valuestring) text = tx->valuestring;
            ESP_LOGI(TAG, "Server: wake word '%s'", text ? text : "");
        } else {
            ESP_LOGI(TAG, "Server listen state='%s'", state ? state : "null");
        }
    }
    else if (strcmp(type, "iot") == 0) {
        // IoT 物模型消息 — 记录但不处理
        char *raw = cJSON_PrintUnformatted(r);
        ESP_LOGI(TAG, "IoT msg: %s", raw ? raw : "(null)");
        free(raw);
    }
    else {
        // 未知消息类型 — 记录原始内容以便调试
        char *raw = cJSON_PrintUnformatted(r);
        ESP_LOGW(TAG, "Unknown msg type='%s': %s", type, raw ? raw : "(null)");
        free(raw);
    }

    cJSON_Delete(r);
}



static void on_binary(const uint8_t *data, int len)
{
    // 不在此处解码/播放（会阻塞 WS 任务 → lwip socket 冲突）
    // 将 Opus 数据入队，由 audio_player_task 异步处理
    if (len <= 0 || len > 256) return;
    audio_frame_t frame;
    memcpy(frame.data, data, len);
    frame.len = len;
    xQueueSend(g_audio_queue, &frame, 0);
}



static void ws_cb(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)data;

    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WS connected, queueing hello...");
        g_reconnect_attempts = 0;
        g_reconnect_pending  = false;
        /* ws_cb 运行在 WS 内部任务上下文，直接取当前任务句柄 */
        g_xz_ws_task = xTaskGetCurrentTaskHandle();
        /* hello 改为队列发送，不在 WS 回调中直接发送，避免 TLS context 重入 */
        {
            defer_msg_t dmsg = { .action = DEFER_SEND_HELLO };
            xQueueSend(g_defer_queue, &dmsg, 0);
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WS disconnected (data_len=%d, payload=%.*s)",
                 d->data_len, d->data_len > 0 ? d->data_len : 0,
                 d->data_ptr ? (const char *)d->data_ptr : "");
        xEventGroupClearBits(g_evtgrp, BIT_CONNECTED | BIT_LISTENING | BIT_MCP_DONE);
        g_am_listening = false;
        notify(XIAOZHI_EVENT_DISCONNECTED, NULL);
        /* 主动重连：防止 CLOSED 事件重复入队 */
        if (!g_reconnect_pending) {
            g_reconnect_pending = true;
            defer_msg_t dmsg = { .action = DEFER_RECONNECT };
            xQueueSend(g_defer_queue, &dmsg, 0);
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x01)     // text
            ESP_LOGI(TAG, "WS text: %.*s", d->data_len, d->data_ptr);
        ESP_LOGI(TAG, "WS data op=%02x len=%d", d->op_code, d->data_len);
        if (d->op_code == 0x01)     // text
            on_json(d->data_ptr, d->data_len);
        else if (d->op_code == 0x02) // binary
            on_binary((const uint8_t *)d->data_ptr, d->data_len);
        else if (d->op_code == 0x08) { // close frame
            // 解析 close code (前2字节, big-endian)
            uint16_t close_code = 0;
            if (d->data_len >= 2) {
                close_code = ((uint8_t)d->data_ptr[0] << 8) | (uint8_t)d->data_ptr[1];
            }
            ESP_LOGW(TAG, "WS close frame: code=%d, reason=%.*s",
                     close_code,
                     d->data_len > 2 ? d->data_len - 2 : 0,
                     d->data_len > 2 ? (const char *)d->data_ptr + 2 : "");
            if (close_code == 1008) { // Policy Violation — 认证失败
                ESP_LOGE(TAG, "认证失败(code 1008)！请确认：");
                ESP_LOGE(TAG, "  1. 已在 https://xiaozhi.me 注册并绑定设备");
                ESP_LOGE(TAG, "  2. 设备已激活（检查OTA返回的activation字段）");
                ESP_LOGE(TAG, "  3. access_token 已正确配置");
            } else if (close_code == 4401) { // 自定义: 未授权
                ESP_LOGE(TAG, "未授权(4401) — 设备需要激活");
            }
            /* 立即停止音频发送，避免断连期间持续发送导致错误日志泛滥 */
            xEventGroupClearBits(g_evtgrp, BIT_CONNECTED | BIT_LISTENING | BIT_MCP_DONE);
            g_am_listening = false;
        }
        else if (d->op_code == 0x09) // ping
            ESP_LOGI(TAG, "WS ping");
        else if (d->op_code == 0x0A) // pong
            ESP_LOGI(TAG, "WS pong");
        else
            ESP_LOGI(TAG, "WS unknown op=0x%02x", d->op_code);
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGE(TAG, "WS error (data=%.*s)",
                 d->data_len > 0 ? d->data_len : 0,
                 d->data_ptr ? (const char *)d->data_ptr : "");
        notify(XIAOZHI_EVENT_ERROR, "websocket error");
        break;

    case WEBSOCKET_EVENT_CLOSED:
        ESP_LOGW(TAG, "WS closed");
        /* 清除事件位停止音频发送，通知上层断连 */
        xEventGroupClearBits(g_evtgrp, BIT_CONNECTED | BIT_LISTENING | BIT_MCP_DONE);
        g_am_listening = false;
        notify(XIAOZHI_EVENT_DISCONNECTED, NULL);
        /* 主动重连（DISCONNECTED 通常已入队，此作兜底）*/
        if (!g_reconnect_pending) {
            g_reconnect_pending = true;
            defer_msg_t dmsg = { .action = DEFER_RECONNECT };
            xQueueSend(g_defer_queue, &dmsg, 0);
        }
        break;

    default:
        ESP_LOGI(TAG, "WS event %" PRId32, id);
        break;
    }
}



static void capture_task(void *arg)
{
    /* Opus 编码器内部 DSP 运算栈开销大（~15KB），大数组必须放 PSRAM 而非栈 */
    int16_t *pcm = heap_caps_malloc(OPUS_FRAME_SAMPLES * sizeof(int16_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *opus = heap_caps_malloc(OPUS_MAX_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm || !opus) {
        ESP_LOGE(TAG, "capture_task: alloc failed");
        if (pcm) free(pcm);
        if (opus) free(opus);
        vTaskDelete(NULL);
        return;
    }

    int send_errors = 0;

    while (1) {
        // 等待同时满足: 已连接 且 正在聆听 且 MCP 握手完成
        xEventGroupWaitBits(g_evtgrp,
            BIT_CONNECTED | BIT_LISTENING | BIT_MCP_DONE,
            pdFALSE, pdTRUE, portMAX_DELAY);

        // 使用 voice 模块的共享环形缓冲区读取音频
        int rd = voice_read_audio(pcm, OPUS_FRAME_SAMPLES,
                                   pdMS_TO_TICKS(OPUS_FRAME_MS + 20));
        if (rd < OPUS_FRAME_SAMPLES) {
            /* 读超时 → 检查连接是否还在，不在则回到等待 */
            if (!(xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED)) continue;
            /* 检查聆听位是否被服务器清除 */
            if (!(xEventGroupGetBits(g_evtgrp) & BIT_LISTENING)) continue;
            continue;
        }

        /* 编码前再次确认连接有效且仍在聆听状态 */
        EventBits_t cur_bits = xEventGroupGetBits(g_evtgrp);
        if (!(cur_bits & BIT_CONNECTED)) continue;
        if (!(cur_bits & BIT_LISTENING)) continue;   // 服务器可能已发 listen stop

        int enc = opus_encode(g_enc, pcm, OPUS_FRAME_SAMPLES,
                               opus, OPUS_MAX_BYTES);
        if (enc <= 0) continue;

        esp_err_t send_ret = safe_ws_bin_send(opus, (uint16_t)enc);
        if (send_ret != ESP_OK) {
            send_errors++;
            if (send_errors >= 5) {
                /* 连续发送失败 → 断连，回到等待状态 */
                ESP_LOGW(TAG, "capture: %d send errors, stopping", send_errors);
                g_am_listening = false;
                xEventGroupClearBits(g_evtgrp, BIT_LISTENING);
                send_errors = 0;
                continue;
            }
        } else {
            send_errors = 0;
        }
    }
}



esp_err_t xiaozhi_ai_init(const xiaozhi_ai_config_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "WiFi not connected");
        return ESP_ERR_INVALID_STATE;
    }
    if (!voice_get_rx_channel()) {
        ESP_LOGE(TAG, "Voice module not inited");
        return ESP_ERR_INVALID_STATE;
    }

    g_cfg = *cfg;


    client_id_init();

    // 先从 NVS 加载缓存的 OTA 结果（避免每次重启都发 HTTP 请求）
    {
        nvs_handle_t nvs;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            size_t len;
            len = sizeof(g_ota_ws_url);
            nvs_get_str(nvs, "ota_ws_url", g_ota_ws_url, &len);
            len = sizeof(g_ota_token);
            nvs_get_str(nvs, "ota_token", g_ota_token, &len);
            len = sizeof(g_activation_code);
            nvs_get_str(nvs, NVS_KEY_ACT_CODE, g_activation_code, &len);
            nvs_close(nvs);
        }
    }

    bool have_cached = (g_ota_ws_url[0] || g_ota_token[0]);
    if (!have_cached) {
        // 新设备首次注册 — 需要 OTA HTTP 请求
        ESP_LOGI(TAG, "OTA: No cache, requesting from server...");
        ota_register();
    } else {
        ESP_LOGI(TAG, "OTA: Using NVS cached URL=%s and cached token",
                 g_ota_ws_url);
    }

    // 检查是否有可用的 token（OTA 返回的或缓存的），没有就用 test-token 兜底
    bool have_token = false;
    {
        if (cfg->access_token && cfg->access_token[0]) {
            have_token = true;
        } else {
            nvs_handle_t nvs;
            if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                static char nvs_token[256];
                size_t len = sizeof(nvs_token);
                if (nvs_get_str(nvs, NVS_KEY_WS_TOKEN, nvs_token, &len) == ESP_OK && nvs_token[0]) {
                    have_token = true;
                }
                nvs_close(nvs);
            }
            if (!have_token && g_ota_token[0]) {
                have_token = true;
            }
            // 兜底：用 test-token（OTA 返回的也是这个），确保小智 AI 可用
            if (!have_token) {
                strncpy(g_ota_token, "test-token", sizeof(g_ota_token) - 1);
                have_token = true;
                ESP_LOGI(TAG, "Using fallback test-token");
            }
        }
    }

    /* event group */
    g_evtgrp = xEventGroupCreate();
    if (!g_evtgrp) return ESP_ERR_NO_MEM;

    /* 延迟发送队列 — 所有 WS 发送操作序列化到独立任务 */
    g_defer_queue = xQueueCreate(16, sizeof(defer_msg_t));
    if (!g_defer_queue) { xiaozhi_ai_deinit(); return ESP_ERR_NO_MEM; }
    if (xTaskCreateWithCaps(sender_task, "xz_send", 20480, NULL, 2, &g_defer_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xiaozhi_ai_deinit();
        return ESP_ERR_NO_MEM;
    }

    /* 音频播放队列 — Opus 解码 + I2S 输出独立任务，避免阻塞 WS 任务 */
    g_audio_queue = xQueueCreate(16, sizeof(audio_frame_t));
    if (!g_audio_queue) { xiaozhi_ai_deinit(); return ESP_ERR_NO_MEM; }
    /* Opus */
    if (opus_init() != ESP_OK) { xiaozhi_ai_deinit(); return ESP_FAIL; }
    if (xTaskCreateWithCaps(audio_player_task, "xz_audio", 20480, NULL, 2, &g_audio_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xiaozhi_ai_deinit();
        return ESP_ERR_NO_MEM;
    }

    {
        const char *url = cfg->server_url ? cfg->server_url
                        : (g_ota_ws_url[0] ? g_ota_ws_url : DEFAULT_SERVER_URL);
        ESP_LOGI(TAG, "Using WebSocket transport (url=%s)", url);

        // 诊断：打印当前内部堆剩余
        ESP_LOGI(TAG, "Before WS task create — internal free: %u, largest block: %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        // DNS 预解析 — 避免 TLS 握手时卡在 DNS
        {
            struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
            struct addrinfo *res = NULL;
            // 从 URL 提取 host（去掉 wss:// 前缀）
            const char *host_start = strstr(url, "://");
            host_start = host_start ? host_start + 3 : url;
            char host[128] = {0};
            const char *slash = strchr(host_start, '/');
            int host_len = slash ? (int)(slash - host_start) : (int)strlen(host_start);
            if (host_len >= (int)sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, host_start, host_len);

            ESP_LOGI(TAG, "DNS pre-resolve: %s", host);
            if (getaddrinfo(host, "443", &hints, &res) == 0 && res) {
                char ip[INET_ADDRSTRLEN];
                struct sockaddr_in *a = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
                ESP_LOGI(TAG, "DNS OK: %s → %s", host, ip);
                freeaddrinfo(res);
            } else {
                ESP_LOGW(TAG, "DNS FAIL for %s — TLS may timeout", host);
            }
        }

        // 释放 OTA 占用的内部 RAM 后，等一下让堆合并碎片
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP_LOGI(TAG, "After OTA cleanup — internal free: %u, largest: %u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

        esp_websocket_client_config_t wc = {
            .uri                = url,
            .transport          = WEBSOCKET_TRANSPORT_OVER_SSL,
            .task_name          = "xz_ws",
            .task_prio          = 5,
            .task_stack         = 28672,        // TLS + cJSON 递归解析需要充足栈空间，避免栈溢出
            .buffer_size        = 4096 + 1,     // >4096 → 从 PSRAM 分配 rx/tx buffer，释放内部 RAM 给 TLS
            .task_core_id_set   = false,
            .reconnect_timeout_ms = 0,            // 禁用内置重连，由 sender_task 指数退避统一管理
            .network_timeout_ms   = 60000,      // TLS 握手在内部 RAM 紧张时可能很慢
            .crt_bundle_attach  = esp_crt_bundle_attach,
        };
        g_ws = esp_websocket_client_init(&wc);
        if (!g_ws) {
            ESP_LOGE(TAG, "WebSocket client allocation failed");
            xiaozhi_ai_deinit();
            return ESP_ERR_NO_MEM;
        }

        const char *tok = NULL;
        if (cfg->access_token && cfg->access_token[0]) {
            tok = cfg->access_token;
        } else {
            nvs_handle_t nvs;
            if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
                static char nvs_token[256];
                size_t len = sizeof(nvs_token);
                if (nvs_get_str(nvs, NVS_KEY_WS_TOKEN, nvs_token, &len) == ESP_OK
                    && nvs_token[0]) {
                    tok = nvs_token;
                }
                nvs_close(nvs);
            }
            if (!tok && g_ota_token[0]) {
                tok = g_ota_token;
            }
        }
        if (tok) {
            ESP_LOGI(TAG, "Using configured authentication token");
            char hdr[320];
            snprintf(hdr, sizeof(hdr), "Bearer %s", tok);
            esp_websocket_client_append_header(g_ws, "Authorization", hdr);
        }

        esp_websocket_client_append_header(g_ws, "Protocol-Version", "1");

        uint8_t mac[6];
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            char device_id[18];
            snprintf(device_id, sizeof(device_id),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            esp_websocket_client_append_header(g_ws, "Device-Id", device_id);
        }
        esp_websocket_client_append_header(g_ws, "Client-Id", g_client_id);

        esp_websocket_register_events(g_ws, WEBSOCKET_EVENT_ANY, ws_cb, NULL);
        esp_err_t ws_start = esp_websocket_client_start(g_ws);
        if (ws_start != ESP_OK) {
            ESP_LOGE(TAG, "WebSocket start failed: %s", esp_err_to_name(ws_start));
            esp_websocket_client_destroy(g_ws);
            g_ws = NULL;
            xiaozhi_ai_deinit();
            return ESP_FAIL;
        }
    }

    /* capture task — Opus 编码器内部 DSP 运算需大量栈，大数组已移入 PSRAM */
    if (xTaskCreateWithCaps(capture_task, "xz_cap", 28672, NULL, 2, &g_cap_task,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        xiaozhi_ai_deinit();
        return ESP_ERR_NO_MEM;
    }

    g_inited = true;
    ESP_LOGI(TAG, "Xiaozhi AI ready (WebSocket)");
    return ESP_OK;
}

bool xiaozhi_ai_is_ready(void)
{
    /* 返回 true = 允许 MQTT 并发，false = 暂停 MQTT（避免 TLS 竞争）
     * 逻辑：
     *   小智未连接     → true  （MQTT 正常工作）
     *   小智已连接 + MCP 握手期间 → false （暂停 MQTT）
     *   小智已连接 + MCP 完成 5 秒内 → false （冷却期）
     *   小智已连接 + MCP 完成 5 秒后 → true  （恢复 MQTT）
     */
    if (!g_evtgrp) return true;
    EventBits_t bits = xEventGroupGetBits(g_evtgrp);
    if (!(bits & BIT_CONNECTED)) return true;
    if (!(bits & BIT_MCP_DONE)) return false;

    static TickType_t s_mcp_done_tick = 0;
    static bool s_mcp_was_done = false;
    bool mcp_done = (bits & BIT_MCP_DONE) != 0;
    if (mcp_done && !s_mcp_was_done) {
        s_mcp_done_tick = xTaskGetTickCount();
        s_mcp_was_done = true;
    } else if (!mcp_done) {
        s_mcp_was_done = false;
    }
    return (xTaskGetTickCount() - s_mcp_done_tick) > pdMS_TO_TICKS(5000);
}

void xiaozhi_ai_deinit(void)
{
    g_inited = false;
    g_am_listening = false;
    g_xz_ws_task = NULL;

    if (g_defer_task) { vTaskDelete(g_defer_task); g_defer_task = NULL; }
    if (g_defer_queue) { vQueueDelete(g_defer_queue); g_defer_queue = NULL; }
    if (g_audio_task) { vTaskDelete(g_audio_task); g_audio_task = NULL; }
    if (g_audio_queue) { vQueueDelete(g_audio_queue); g_audio_queue = NULL; }
    if (g_cap_task) { vTaskDelete(g_cap_task); g_cap_task = NULL; }
    if (g_ws)       { esp_websocket_client_stop(g_ws);
                      esp_websocket_client_destroy(g_ws); g_ws = NULL; }
    if (g_evtgrp)   { vEventGroupDelete(g_evtgrp); g_evtgrp = NULL; }
    opus_deinit();
    ESP_LOGI(TAG, "Deinit done");
}

esp_err_t xiaozhi_ai_start_listen(void)
{
    if (!g_inited || !g_ws) return ESP_ERR_INVALID_STATE;
    if (!(xEventGroupGetBits(g_evtgrp) & BIT_CONNECTED)) {
        ESP_LOGW(TAG, "Not connected");
        return ESP_ERR_INVALID_STATE;
    }
    send_listen("start", (g_cfg.mode == XIAOZHI_MODE_AUTO) ? "auto" : "manual");
    g_am_listening = true;
    xEventGroupSetBits(g_evtgrp, BIT_LISTENING);
    notify(XIAOZHI_EVENT_LISTENING, NULL);
    ESP_LOGI(TAG, "Listening on");
    return ESP_OK;
}

esp_err_t xiaozhi_ai_stop_listen(void)
{
    if (!g_inited) return ESP_ERR_INVALID_STATE;
    g_am_listening = false;
    xEventGroupClearBits(g_evtgrp, BIT_LISTENING);
    send_listen("stop", NULL);
    notify(XIAOZHI_EVENT_THINKING, NULL);
    ESP_LOGI(TAG, "Listening off");
    return ESP_OK;
}

esp_err_t xiaozhi_ai_wake_word_detected(const char *text)
{
    if (!g_inited || !g_defer_queue) return ESP_ERR_INVALID_STATE;
    defer_msg_t dmsg = { .action = DEFER_ABORT_AND_WAKE };
    if (text) strncpy(dmsg.text, text, sizeof(dmsg.text) - 1);
    if (xQueueSend(g_defer_queue, &dmsg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Wake queue full");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "Wake queued: %s", text ? text : "");
    return ESP_OK;
}

esp_err_t xiaozhi_ai_abort(void)
{
    if (!g_inited) return ESP_ERR_INVALID_STATE;
    g_am_listening = false;
    xEventGroupClearBits(g_evtgrp, BIT_LISTENING);
    send_abort_msg();
    return ESP_OK;
}

esp_err_t xiaozhi_ai_send_text(const char *text)
{
    if (!text || !text[0]) return ESP_ERR_INVALID_ARG;
    if (!g_inited || !g_defer_queue) return ESP_ERR_INVALID_STATE;
    defer_msg_t dmsg = {
        .action = DEFER_SEND_TEXT,
    };
    strncpy(dmsg.text, text, sizeof(dmsg.text) - 1);
    dmsg.text[sizeof(dmsg.text) - 1] = '\0';
    return xQueueSend(g_defer_queue, &dmsg, pdMS_TO_TICKS(100)) == pdTRUE
               ? ESP_OK : ESP_ERR_TIMEOUT;
}

const char *xiaozhi_ai_get_activation_code(void)
{
    if (g_activation_code[0]) return g_activation_code;
    return NULL;
}

void xiaozhi_ai_mute_tts(bool mute)
{
    g_tts_muted = mute;
    ESP_LOGI(TAG, "TTS %s", mute ? "muted" : "unmuted");
}
