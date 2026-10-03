#include "ble.h"
#include "esp_log.h"
#include "esp_bt.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/dis/ble_svc_dis.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "BLE";

// ---- 连接状态 ----
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool     g_ble_ready   = false;

// ---- 传感器数据缓存 ----
static float    g_temp  = -99.0f;
static float    g_humi  = -99.0f;
static float    g_lux   = 500.0f;
static uint16_t g_tvoc  = 0;
static uint16_t g_co2   = 0;
static bool     g_wifi_connected = false;
static ble_ctrl_cb_t g_ctrl_cb = NULL;

// ---- Notify 支持 ----
// 特征值句柄由 NimBLE 在 ble_gatts_start() 时写入，CCCD 句柄 = 值句柄 + 1
#define CHR_IDX_TEMP   0
#define CHR_IDX_HUMI   1
#define CHR_IDX_LIGHT  2
#define CHR_IDX_TVOC   3
#define CHR_IDX_CO2    4
#define CHR_IDX_WIFI   5
#define CHR_IDX_NUM    6
static uint16_t g_chr_val_handle[CHR_IDX_NUM] = {0};
static bool     g_chr_notify_on[CHR_IDX_NUM]  = {false};

// ---- 自定义 UUID ----
// Custom Service: 0x6E40
// Characteristics: 0x6E41 ~ 0x6E47
#define CUSTOM_SVC_UUID      0x6E40
#define CHAR_UUID_TEMP       0x6E41
#define CHAR_UUID_HUMI       0x6E42
#define CHAR_UUID_LIGHT      0x6E43
#define CHAR_UUID_TVOC       0x6E44
#define CHAR_UUID_CO2        0x6E45
#define CHAR_UUID_CTRL       0x6E46
#define CHAR_UUID_WIFI       0x6E47

// ---- 读写回调 ----
static int sensor_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t uuid = (uint16_t)(uintptr_t)arg;
    char val[32];

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        switch (uuid) {
        case CHAR_UUID_TEMP:  snprintf(val, sizeof(val), "%.1f", g_temp);  break;
        case CHAR_UUID_HUMI:  snprintf(val, sizeof(val), "%.1f", g_humi);  break;
        case CHAR_UUID_LIGHT: snprintf(val, sizeof(val), "%.0f", g_lux);   break;
        case CHAR_UUID_TVOC:  snprintf(val, sizeof(val), "%u", g_tvoc);    break;
        case CHAR_UUID_CO2:   snprintf(val, sizeof(val), "%u", g_co2);     break;
        case CHAR_UUID_WIFI:
            snprintf(val, sizeof(val), "%s", g_wifi_connected ? "1" : "0");
            break;
        default: return BLE_ATT_ERR_INVALID_HANDLE;
        }
        os_mbuf_append(ctxt->om, val, strlen(val));
        return 0;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && uuid == CHAR_UUID_CTRL) {
        char cmd[64] = {0};
        int len = OS_MBUF_PKTLEN(ctxt->om);
        if (len > (int)sizeof(cmd) - 1) len = sizeof(cmd) - 1;
        ble_hs_mbuf_to_flat(ctxt->om, cmd, len, NULL);
        cmd[len] = '\0';
        ESP_LOGI(TAG, "CTRL write: %s", cmd);
        if (g_ctrl_cb) g_ctrl_cb(cmd);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

// ---- GATT Service 表 ----
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(CUSTOM_SVC_UUID),
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_TEMP),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_TEMP,
                .val_handle = &g_chr_val_handle[CHR_IDX_TEMP],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_HUMI),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_HUMI,
                .val_handle = &g_chr_val_handle[CHR_IDX_HUMI],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_LIGHT),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_LIGHT,
                .val_handle = &g_chr_val_handle[CHR_IDX_LIGHT],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_TVOC),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_TVOC,
                .val_handle = &g_chr_val_handle[CHR_IDX_TVOC],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_CO2),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_CO2,
                .val_handle = &g_chr_val_handle[CHR_IDX_CO2],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_WIFI),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_WIFI,
                .val_handle = &g_chr_val_handle[CHR_IDX_WIFI],
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            },
            {
                .uuid = BLE_UUID16_DECLARE(CHAR_UUID_CTRL),
                .access_cb = sensor_access_cb,
                .arg = (void *)(uintptr_t)CHAR_UUID_CTRL,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {0}  // sentinel
        },
    },
    {0}  // sentinel
};

// ---- 默认广播参数（不能传 NULL，NimBLE 会返回 EINVAL）----
static struct ble_gap_adv_params g_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_UND,
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
};

// ---- GAP 事件回调 ----
static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Connected (handle=%d)", g_conn_handle);
        } else {
            ESP_LOGW(TAG, "Connect failed (status=%d)", event->connect.status);
            ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                              &g_adv_params, gap_event_cb, NULL);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Disconnected (reason=%d)", event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        // 连接断开后客户端的订阅全部失效，清空以免向已断开的连接推送
        memset(g_chr_notify_on, 0, sizeof(g_chr_notify_on));
        ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                          &g_adv_params, gap_event_cb, NULL);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        // 注意：这里的 attr_handle 是"特征值句柄"（CCCD 句柄 = 值句柄 + 1）
        for (int i = 0; i < CHR_IDX_NUM; i++) {
            if (g_chr_val_handle[i] != 0 &&
                event->subscribe.attr_handle == g_chr_val_handle[i]) {
                g_chr_notify_on[i] = (event->subscribe.cur_notify != 0);
                ESP_LOGI(TAG, "Notify %s (chr idx=%d, conn=%d, reason=%d)",
                         g_chr_notify_on[i] ? "enabled" : "disabled", i,
                         event->subscribe.conn_handle, event->subscribe.reason);
                break;
            }
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update: conn=%d mtu=%d", event->mtu.conn_handle, event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

// ---- Host + Controller sync 回调 ----
static void on_sync(void)
{
    // 先初始化 NimBLE 内置服务（必须在注册自定义服务之前）
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_dis_init();

    // 设置设备名
    ble_svc_gap_device_name_set("AIoT-SmartHome");

    // 注册 GATT 服务
    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_count_cfg: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_add_svcs: %d", rc);
        return;
    }

    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gatts_start: %d", rc);
        return;
    }

    // 开始广播
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &g_adv_params, gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start: %d", rc);
        return;
    }

    g_ble_ready = true;
    ESP_LOGI(TAG, "BLE ready, advertising as 'AIoT-SmartHome'");
}

static void nimble_host_task(void *arg)
{
    nimble_port_run();
    nimble_port_deinit();
    vTaskDelete(NULL);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (reason=%d), restarting...", reason);
}

// ---- 初始化 ----
void ble_init(void)
{
    // 设置 NimBLE 回调（必须在 nimble_port_init() 之前）
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    // nimble_port_init() 一站式初始化：
    //   释放经典蓝牙内存 → 初始化 BLE 控制器 → 使能 BLE → 初始化 NimBLE host
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    xTaskCreate(nimble_host_task, "nimble_host", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "BLE init OK");
}

// ---- 传感器数据更新（更新缓存值并推送 Notify）----

/* 向已订阅的客户端推送字符串通知 */
static void ble_notify_chr(int idx, const char *value)
{
    if (idx < 0 || idx >= CHR_IDX_NUM) return;
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE) return;
    if (!g_chr_notify_on[idx] || g_chr_val_handle[idx] == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(value, (uint16_t)strlen(value));
    if (om == NULL) {
        ESP_LOGW(TAG, "Notify: mbuf unavailable (MSYS pool exhausted)");
        return;
    }
    /* ble_gatts_notify_custom 无论成功失败都会消费这个 mbuf，这里不能再 free */
    int rc = ble_gatts_notify_custom(g_conn_handle, g_chr_val_handle[idx], om);
    if (rc != 0) {
        ESP_LOGW(TAG, "Notify chr idx=%d failed: %d", idx, rc);
    }
}

void ble_update_temperature(float temp) {
    char val[16];
    g_temp = temp;
    snprintf(val, sizeof(val), "%.1f", temp);
    ble_notify_chr(CHR_IDX_TEMP, val);
}

void ble_update_humidity(float humi) {
    char val[16];
    g_humi = humi;
    snprintf(val, sizeof(val), "%.1f", humi);
    ble_notify_chr(CHR_IDX_HUMI, val);
}

void ble_update_light(float lux) {
    char val[16];
    g_lux = lux;
    snprintf(val, sizeof(val), "%.0f", lux);
    ble_notify_chr(CHR_IDX_LIGHT, val);
}

void ble_update_tvoc(uint16_t tvoc) {
    char val[16];
    g_tvoc = tvoc;
    snprintf(val, sizeof(val), "%u", (unsigned)tvoc);
    ble_notify_chr(CHR_IDX_TVOC, val);
}

void ble_update_co2(uint16_t co2) {
    char val[16];
    g_co2 = co2;
    snprintf(val, sizeof(val), "%u", (unsigned)co2);
    ble_notify_chr(CHR_IDX_CO2, val);
}

void ble_set_wifi_status(bool connected) {
    g_wifi_connected = connected;
    ble_notify_chr(CHR_IDX_WIFI, connected ? "1" : "0");
}

void ble_set_control_callback(ble_ctrl_cb_t cb) { g_ctrl_cb = cb; }
