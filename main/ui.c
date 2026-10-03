#include "ui.h"
#include "font_cjk_16.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

static const char *TAG = "UI";

// 全局UI控件句柄
static lv_obj_t *tabview;
static lv_obj_t *tab_sensors;
static lv_obj_t *tab_network;
static lv_obj_t *tab_system;

// 状态栏控件
static lv_obj_t *status_bar;
static lv_obj_t *label_wifi_icon;
static lv_obj_t *label_clock;

// 传感器标签句柄
static lv_obj_t *label_temp;
static lv_obj_t *label_humi;
static lv_obj_t *label_light;
static lv_obj_t *label_tvoc;
static lv_obj_t *label_co2;

// 网络状态标签句柄
static lv_obj_t *label_wifi_status;
static lv_obj_t *label_mqtt_status;

// 系统信息标签句柄
static lv_obj_t *label_uptime;
static lv_obj_t *label_heap;

// 小智AI状态标签
static lv_obj_t *label_ai_status;

// 小智AI回复标签（展示完整的语音回复文本）
static lv_obj_t *label_ai_reply;

// WiFi 连接状态缓存（供状态栏使用）
static bool g_wifi_connected = false;

// 系统启动时间
static time_t start_time;

// 给卡片添加统一的阴影样式
static void style_card_shadow(lv_obj_t *card) {
    lv_obj_set_style_shadow_color(card, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(card, 8, 0);
    lv_obj_set_style_shadow_ofs_x(card, 2, 0);
    lv_obj_set_style_shadow_ofs_y(card, 3, 0);
}

// 创建图标+文字水平排列行（图标=montserrat, 文字=默认simsun）
static lv_obj_t* make_icon_text(lv_obj_t *parent, const char *symbol, const char *text, lv_color_t color) {
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(row);
    lv_label_set_text(icon, symbol);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(icon, color, 0);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_cjk_16, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return row;
}

// 创建常驻状态栏（最顶部，所有页面可见）
static void create_status_bar(void) {
    status_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(status_bar, 320, 30);
    lv_obj_align(status_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(status_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(status_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(status_bar, 10, 0);
    lv_obj_set_style_pad_right(status_bar, 10, 0);

    // 左侧：WiFi 图标
    label_wifi_icon = lv_label_create(status_bar);
    lv_label_set_text(label_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(label_wifi_icon, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(label_wifi_icon, &lv_font_montserrat_16, 0);

    // 右侧：时钟
    label_clock = lv_label_create(status_bar);
    lv_label_set_text(label_clock, "00:00");
    lv_obj_set_style_text_color(label_clock, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(label_clock, &lv_font_montserrat_16, 0);
}

// 状态栏时钟更新定时器
static void status_bar_timer_cb(lv_timer_t *timer) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char buf[8];
    sprintf(buf, "%02d:%02d", t->tm_hour, t->tm_min);
    lv_label_set_text(label_clock, buf);
}

// 创建传感器页面
static void create_chat_tab(lv_obj_t *parent);

static void create_sensors_tab(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 6, 0);

    // 卡片尺寸与间距
    const int card_w = 143;
    const int card_h = 72;
    const int card_gap = 6;
    const int start_y = 4;
    const int col_left = 6;
    const int col_right = 155;

    // 温度卡片（蓝色）
    lv_obj_t *cont_temp = lv_obj_create(parent);
    lv_obj_set_size(cont_temp, card_w, card_h);
    lv_obj_align(cont_temp, LV_ALIGN_TOP_LEFT, col_left, start_y);
    lv_obj_set_style_radius(cont_temp, 12, 0);
    lv_obj_set_style_bg_color(cont_temp, lv_color_hex(0x2196F3), 0);
    style_card_shadow(cont_temp);

    lv_obj_t *label_temp_title = make_icon_text(cont_temp, LV_SYMBOL_CHARGE, " 温度", lv_color_white());
    lv_obj_align(label_temp_title, LV_ALIGN_TOP_MID, 0, 6);

    label_temp = lv_label_create(cont_temp);
    lv_label_set_text(label_temp, "--.-°C");
    lv_obj_align(label_temp, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_font(label_temp, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_temp, lv_color_white(), 0);

    // 湿度卡片（绿色）
    lv_obj_t *cont_humi = lv_obj_create(parent);
    lv_obj_set_size(cont_humi, card_w, card_h);
    lv_obj_align(cont_humi, LV_ALIGN_TOP_LEFT, col_right, start_y);
    lv_obj_set_style_radius(cont_humi, 12, 0);
    lv_obj_set_style_bg_color(cont_humi, lv_color_hex(0x4CAF50), 0);
    style_card_shadow(cont_humi);

    lv_obj_t *label_humi_title = make_icon_text(cont_humi, LV_SYMBOL_TINT, " 湿度", lv_color_white());
    lv_obj_align(label_humi_title, LV_ALIGN_TOP_MID, 0, 6);

    label_humi = lv_label_create(cont_humi);
    lv_label_set_text(label_humi, "--.-%");
    lv_obj_align(label_humi, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_font(label_humi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_humi, lv_color_white(), 0);

    // 光照卡片（橙色）
    int row2_y = start_y + card_h + card_gap;
    lv_obj_t *cont_light = lv_obj_create(parent);
    lv_obj_set_size(cont_light, card_w, card_h);
    lv_obj_align(cont_light, LV_ALIGN_TOP_LEFT, col_left, row2_y);
    lv_obj_set_style_radius(cont_light, 12, 0);
    lv_obj_set_style_bg_color(cont_light, lv_color_hex(0xFF9800), 0);
    style_card_shadow(cont_light);

    lv_obj_t *label_light_title = make_icon_text(cont_light, LV_SYMBOL_EYE_OPEN, " 光照", lv_color_white());
    lv_obj_align(label_light_title, LV_ALIGN_TOP_MID, 0, 6);

    label_light = lv_label_create(cont_light);
    lv_label_set_text(label_light, "--- lx");
    lv_obj_align(label_light, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_font(label_light, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_light, lv_color_white(), 0);

    // TVOC卡片（紫色）
    lv_obj_t *cont_tvoc = lv_obj_create(parent);
    lv_obj_set_size(cont_tvoc, card_w, card_h);
    lv_obj_align(cont_tvoc, LV_ALIGN_TOP_LEFT, col_right, row2_y);
    lv_obj_set_style_radius(cont_tvoc, 12, 0);
    lv_obj_set_style_bg_color(cont_tvoc, lv_color_hex(0x9C27B0), 0);
    style_card_shadow(cont_tvoc);

    lv_obj_t *label_tvoc_title = make_icon_text(cont_tvoc, LV_SYMBOL_WARNING, " TVOC", lv_color_white());
    lv_obj_align(label_tvoc_title, LV_ALIGN_TOP_MID, 0, 6);

    label_tvoc = lv_label_create(cont_tvoc);
    lv_label_set_text(label_tvoc, "--- ppb");
    lv_obj_align(label_tvoc, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_text_font(label_tvoc, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(label_tvoc, lv_color_white(), 0);

    // CO2 卡片（红色，底部通栏）
    int row3_y = row2_y + card_h + card_gap;
    lv_obj_t *cont_co2 = lv_obj_create(parent);
    lv_obj_set_size(cont_co2, 292, 52);
    lv_obj_align(cont_co2, LV_ALIGN_TOP_LEFT, col_left, row3_y);
    lv_obj_set_style_radius(cont_co2, 12, 0);
    lv_obj_set_style_bg_color(cont_co2, lv_color_hex(0xE53935), 0);
    style_card_shadow(cont_co2);

    lv_obj_t *label_co2_title = make_icon_text(cont_co2, LV_SYMBOL_WARNING, " CO2", lv_color_white());
    lv_obj_align(label_co2_title, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_text_color(label_co2_title, lv_color_white(), 0);

    label_co2 = lv_label_create(cont_co2);
    lv_label_set_text(label_co2, "---- ppm");
    lv_obj_align(label_co2, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_font(label_co2, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(label_co2, lv_color_white(), 0);
}

// 创建网络状态页面
static void create_network_tab(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 10, 0);

    // 页面标题
    lv_obj_t *title = make_icon_text(parent, LV_SYMBOL_WIFI, " 网络状态", lv_color_hex(0x000000));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // WiFi状态
    lv_obj_t *cont_wifi = lv_obj_create(parent);
    lv_obj_set_size(cont_wifi, 290, 65);
    lv_obj_align(cont_wifi, LV_ALIGN_TOP_MID, 0, 45);
    lv_obj_set_style_radius(cont_wifi, 12, 0);
    style_card_shadow(cont_wifi);

    lv_obj_t *label_wifi_title = make_icon_text(cont_wifi, LV_SYMBOL_WIFI, " WiFi", lv_color_hex(0x000000));
    lv_obj_align(label_wifi_title, LV_ALIGN_LEFT_MID, 16, 0);

    label_wifi_status = lv_label_create(cont_wifi);
    lv_label_set_text(label_wifi_status, "未连接");
    lv_obj_align(label_wifi_status, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_text_font(label_wifi_status, &font_cjk_16, 0);

    // MQTT状态
    lv_obj_t *cont_mqtt = lv_obj_create(parent);
    lv_obj_set_size(cont_mqtt, 290, 65);
    lv_obj_align(cont_mqtt, LV_ALIGN_TOP_MID, 0, 125);
    lv_obj_set_style_radius(cont_mqtt, 12, 0);
    style_card_shadow(cont_mqtt);

    lv_obj_t *label_mqtt_title = make_icon_text(cont_mqtt, LV_SYMBOL_LOOP, " MQTT", lv_color_hex(0x000000));
    lv_obj_align(label_mqtt_title, LV_ALIGN_LEFT_MID, 16, 0);

    label_mqtt_status = lv_label_create(cont_mqtt);
    lv_label_set_text(label_mqtt_status, "未连接");
    lv_obj_align(label_mqtt_status, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(label_mqtt_status, lv_color_hex(0xF44336), 0);
    lv_obj_set_style_text_font(label_mqtt_status, &font_cjk_16, 0);
}

// 创建系统信息页面
static void create_system_tab(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 10, 0);

    // 页面标题
    lv_obj_t *title = make_icon_text(parent, LV_SYMBOL_SETTINGS, " 系统信息", lv_color_hex(0x000000));
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

    // 运行时间
    lv_obj_t *cont_uptime = lv_obj_create(parent);
    lv_obj_set_size(cont_uptime, 290, 55);
    lv_obj_align(cont_uptime, LV_ALIGN_TOP_MID, 0, 42);
    lv_obj_set_style_radius(cont_uptime, 12, 0);
    style_card_shadow(cont_uptime);

    lv_obj_t *label_uptime_title = make_icon_text(cont_uptime, LV_SYMBOL_CHARGE, " 运行时间", lv_color_hex(0x000000));
    lv_obj_align(label_uptime_title, LV_ALIGN_LEFT_MID, 16, 0);

    label_uptime = lv_label_create(cont_uptime);
    lv_label_set_text(label_uptime, "00:00:00");
    lv_obj_align(label_uptime, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_font(label_uptime, &lv_font_montserrat_16, 0);

    // 可用内存
    lv_obj_t *cont_heap = lv_obj_create(parent);
    lv_obj_set_size(cont_heap, 290, 55);
    lv_obj_align(cont_heap, LV_ALIGN_TOP_MID, 0, 108);
    lv_obj_set_style_radius(cont_heap, 12, 0);
    style_card_shadow(cont_heap);

    lv_obj_t *label_heap_title = make_icon_text(cont_heap, LV_SYMBOL_SD_CARD, " 可用内存", lv_color_hex(0x000000));
    lv_obj_align(label_heap_title, LV_ALIGN_LEFT_MID, 16, 0);

    label_heap = lv_label_create(cont_heap);
    lv_label_set_text(label_heap, "---- KB");
    lv_obj_align(label_heap, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_font(label_heap, &lv_font_montserrat_16, 0);

    // 小智AI状态
    lv_obj_t *cont_ai = lv_obj_create(parent);
    lv_obj_set_size(cont_ai, 290, 55);
    lv_obj_align(cont_ai, LV_ALIGN_TOP_MID, 0, 174);
    lv_obj_set_style_radius(cont_ai, 12, 0);
    lv_obj_set_style_bg_color(cont_ai, lv_color_hex(0x00BCD4), 0);
    style_card_shadow(cont_ai);

    lv_obj_t *label_ai_title = make_icon_text(cont_ai, LV_SYMBOL_AUDIO, " 小智AI", lv_color_white());
    lv_obj_align(label_ai_title, LV_ALIGN_LEFT_MID, 16, 0);

    label_ai_status = lv_label_create(cont_ai);
    lv_label_set_text(label_ai_status, "未连接");
    lv_obj_align(label_ai_status, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_text_color(label_ai_status, lv_color_hex(0xFFEB3B), 0);
    lv_obj_set_style_text_font(label_ai_status, &font_cjk_16, 0);

    // AI回复显示（完整语音回复文本）
    lv_obj_t *cont_ai_reply = lv_obj_create(parent);
    lv_obj_set_size(cont_ai_reply, 290, 90);
    lv_obj_align(cont_ai_reply, LV_ALIGN_TOP_MID, 0, 240);
    lv_obj_set_style_radius(cont_ai_reply, 12, 0);
    lv_obj_set_style_bg_color(cont_ai_reply, lv_color_hex(0x00897B), 0);
    style_card_shadow(cont_ai_reply);
    lv_obj_set_style_pad_all(cont_ai_reply, 8, 0);

    lv_obj_t *label_ai_reply_title = make_icon_text(cont_ai_reply, LV_SYMBOL_AUDIO, " AI回复", lv_color_hex(0xB2DFDB));
    lv_obj_align(label_ai_reply_title, LV_ALIGN_TOP_LEFT, 4, 2);

    label_ai_reply = lv_label_create(cont_ai_reply);
    lv_label_set_text(label_ai_reply, "暂无回复");
    lv_obj_set_style_text_font(label_ai_reply, &font_cjk_16, 0);
    lv_obj_align(label_ai_reply, LV_ALIGN_TOP_LEFT, 4, 22);
    lv_obj_set_size(label_ai_reply, 274, 60);
    lv_obj_set_style_text_color(label_ai_reply, lv_color_white(), 0);
    lv_label_set_long_mode(label_ai_reply, LV_LABEL_LONG_WRAP);  // 自动换行
}

// 系统信息更新定时器
static void system_info_timer_cb(lv_timer_t *timer) {
    // 更新运行时间
    time_t now = time(NULL);
    int seconds = now - start_time;
    int hours = seconds / 3600;
    int minutes = (seconds % 3600) / 60;
    seconds = seconds % 60;

    char buf[32];
    sprintf(buf, "%02d:%02d:%02d", hours, minutes, seconds);
    lv_label_set_text(label_uptime, buf);

    // 更新可用内存
    uint32_t free_heap = esp_get_free_heap_size() / 1024;
    sprintf(buf, "%d KB", free_heap);
    lv_label_set_text(label_heap, buf);
}

// UI初始化
void ui_init(void) {
    // 记录系统启动时间
    start_time = time(NULL);

    // 1. 创建常驻状态栏（最顶部）
    create_status_bar();

    // 2. 创建标签页视图（状态栏下方）
    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 40);
    lv_obj_set_pos(tabview, 0, 30);   // 状态栏占 30px
    lv_obj_set_style_bg_color(lv_tabview_get_tab_btns(tabview), lv_color_hex(0x16213e), 0);
    lv_obj_set_style_text_font(lv_tabview_get_tab_btns(tabview), &font_cjk_16, 0);

    // 创建四个标签页
    tab_sensors = lv_tabview_add_tab(tabview, "传感器");
    tab_network = lv_tabview_add_tab(tabview, "网络");
    tab_system = lv_tabview_add_tab(tabview, "系统");
    lv_obj_t *tab_chat = lv_tabview_add_tab(tabview, "AI对话");

    // 3. 初始化各页面内容
    create_sensors_tab(tab_sensors);
    create_network_tab(tab_network);
    create_system_tab(tab_system);
    create_chat_tab(tab_chat);

    // 4. 创建定时器
    lv_timer_create(system_info_timer_cb, 1000, NULL);   // 系统信息
    lv_timer_create(status_bar_timer_cb, 10000, NULL);    // 状态栏时钟（每10秒刷新即可）

    ESP_LOGI(TAG, "UI initialized successfully");
}

// ================================================================
//  跨任务 UI 更新 —— 通过 lv_async_call 保证线程安全
//  所有被非 LVGL 任务调用的 UI 函数，必须使用此机制
// ================================================================

typedef struct {
    float temp, humi, light;
    uint16_t tvoc, co2;
} sensor_update_data_t;

/** [async] 传感器数据更新（在 LVGL 任务上下文执行）*/
static void ui_update_sensors_async(void *user_data) {
    sensor_update_data_t *d = (sensor_update_data_t *)user_data;
    char buf[32];

    if (d->temp > -40 && d->temp < 80) {
        sprintf(buf, "%.1f°C", d->temp);
        lv_label_set_text(label_temp, buf);
    }
    if (d->humi >= 0 && d->humi <= 100) {
        sprintf(buf, "%.1f%%", d->humi);
        lv_label_set_text(label_humi, buf);
    }
    if (d->light >= 0) {
        sprintf(buf, "%.0f lx", d->light);
        lv_label_set_text(label_light, buf);
    }
    // TVOC：显示 SGP30 实时值；无有效读数时用占位符，避免把"无数据"当成 0 ppb
    if (d->tvoc > 0) {
        sprintf(buf, "%u ppb", (unsigned)d->tvoc);
        lv_label_set_text(label_tvoc, buf);
    } else {
        lv_label_set_text(label_tvoc, "-- ppb");
    }
    if (d->co2 > 0) {
        sprintf(buf, "%d ppm", d->co2);
        lv_label_set_text(label_co2, buf);
    }
    free(d);
}

/** [async] WiFi 状态更新 */
static void ui_update_wifi_status_async(void *user_data) {
    bool connected = (bool)(uintptr_t)user_data;
    g_wifi_connected = connected;
    if (connected) {
        lv_label_set_text(label_wifi_status, "已连接");
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(0x4CAF50), 0);
        lv_obj_set_style_text_color(label_wifi_icon, lv_color_hex(0x4CAF50), 0);
    } else {
        lv_label_set_text(label_wifi_status, "未连接");
        lv_obj_set_style_text_color(label_wifi_status, lv_color_hex(0xF44336), 0);
        lv_obj_set_style_text_color(label_wifi_icon, lv_color_hex(0x666666), 0);
    }
}

/** [async] MQTT 状态更新 */
static void ui_update_mqtt_status_async(void *user_data) {
    bool connected = (bool)(uintptr_t)user_data;
    if (connected) {
        lv_label_set_text(label_mqtt_status, "已连接");
        lv_obj_set_style_text_color(label_mqtt_status, lv_color_hex(0x4CAF50), 0);
    } else {
        lv_label_set_text(label_mqtt_status, "未连接");
        lv_obj_set_style_text_color(label_mqtt_status, lv_color_hex(0xF44336), 0);
    }
}

/** [async] AI语音回复更新 */
static void ui_update_ai_reply_async(void *user_data) {
    const char *text = (const char *)user_data;
    if (!text) return;
    if (label_ai_reply) {
        lv_label_set_text(label_ai_reply, text);
    }
    free((void *)text);
}

/** [async] 小智AI 状态更新 */
static void ui_update_ai_status_async(void *user_data) {
    const char *status = (const char *)user_data;
    if (!status) return;

    lv_color_t color;
    if (strstr(status, "聆听")) {
        color = lv_color_hex(0x4CAF50);
    } else if (strstr(status, "思考") || strstr(status, "回复")) {
        color = lv_color_hex(0xFF9800);
    } else if (strstr(status, "已连接")) {
        color = lv_color_hex(0xFFFFFF);
    } else if (strstr(status, "错误") || strstr(status, "断开")) {
        color = lv_color_hex(0xF44336);
    } else {
        color = lv_color_hex(0xFFEB3B);
    }

    if (label_ai_status) {
        lv_label_set_text(label_ai_status, status);
        lv_obj_set_style_text_color(label_ai_status, color, 0);
    }
    free((void *)status);  // 释放 strdup 分配的内存
}

// ---- 公开 API（线程安全，可在任何任务中调用）----

// 更新传感器数据（线程安全）
void ui_update_sensors(float temp, float humi, float light, uint16_t tvoc, uint16_t co2) {
    sensor_update_data_t *d = malloc(sizeof(sensor_update_data_t));
    if (!d) return;
    d->temp = temp; d->humi = humi; d->light = light;
    d->tvoc = tvoc; d->co2  = co2;
    if (lv_async_call(ui_update_sensors_async, d) != LV_RES_OK) free(d);
}

// 更新WiFi状态（线程安全）
void ui_update_wifi_status(bool connected) {
    lv_async_call(ui_update_wifi_status_async, (void *)(uintptr_t)connected);
}

// 更新MQTT状态（线程安全）
void ui_update_mqtt_status(bool connected) {
    lv_async_call(ui_update_mqtt_status_async, (void *)(uintptr_t)connected);
}

static void ui_switch_to_next_page_async(void *arg) {
    (void)arg;
    uint32_t current = lv_tabview_get_tab_act(tabview);
    uint32_t total = ((lv_btnmatrix_t *)lv_tabview_get_tab_btns(tabview))->btn_cnt;

    uint32_t next = (current < total - 1) ? current + 1 : 0;
    lv_tabview_set_act(tabview, next, LV_ANIM_ON);
}

// 切换到下一页（线程安全）
void ui_switch_to_next_page(void) {
    lv_async_call(ui_switch_to_next_page_async, NULL);
}

// 更新小智AI状态（线程安全）
void ui_update_ai_status(const char *status) {
    if (!status) return;
    char *s = strdup(status);
    if (s && lv_async_call(ui_update_ai_status_async, s) != LV_RES_OK) free(s);
}

// 更新AI语音回复（线程安全）
void ui_update_ai_reply(const char *text) {
    if (!text) return;
    char *s = strdup(text);
    if (s && lv_async_call(ui_update_ai_reply_async, s) != LV_RES_OK) free(s);
}

static void ui_switch_to_prev_page_async(void *arg) {
    (void)arg;
    uint32_t current = lv_tabview_get_tab_act(tabview);
    uint32_t total = ((lv_btnmatrix_t *)lv_tabview_get_tab_btns(tabview))->btn_cnt;

    uint32_t prev = (current > 0) ? current - 1 : total - 1;
    lv_tabview_set_act(tabview, prev, LV_ANIM_ON);
}


// 切换到上一页（线程安全）
void ui_switch_to_prev_page(void) {
    lv_async_call(ui_switch_to_prev_page_async, NULL);
}

// ================================================================
//  聊天 Tab（AI对话）—— 预设句子按钮 + AI 回复显示
// ================================================================

static lv_obj_t *chat_msg_area;
static ui_chat_send_cb_t g_chat_send_cb = NULL;
static int g_chat_msg_count = 0;
#define CHAT_MAX_MSGS 20

// 预设句子列表
static const char *preset_sentences[] = {
    "你好小智",
    "今天天气怎么样",
    "现在几点了",
    "帮我查一下温度",
};
#define PRESET_COUNT (sizeof(preset_sentences) / sizeof(preset_sentences[0]))

// 预设按钮点击 → 调用 main.c 注册的发送回调
static void chat_preset_btn_cb(lv_event_t *e) {
    const char *text = (const char *)lv_event_get_user_data(e);
    if (text && g_chat_send_cb) {
        g_chat_send_cb(text);
    }
}

// 异步添加消息到聊天区
typedef struct {
    char text[256];
    bool is_user;
} chat_msg_async_t;

static void chat_add_msg_async(void *user_data) {
    chat_msg_async_t *d = (chat_msg_async_t *)user_data;
    if (!d || !chat_msg_area) { free(d); return; }

    // 限制消息数量
    if (g_chat_msg_count >= CHAT_MAX_MSGS) {
        lv_obj_t *first = lv_obj_get_child(chat_msg_area, 0);
        if (first) lv_obj_del(first);
        g_chat_msg_count--;
    }

    // 消息气泡
    lv_obj_t *bubble = lv_obj_create(chat_msg_area);
    lv_obj_set_size(bubble, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(bubble, 6, 0);
    lv_obj_set_style_radius(bubble, 8, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);

    if (d->is_user) {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x2196F3), 0);
        lv_obj_set_style_text_color(bubble, lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(bubble, lv_color_hex(0x37474F), 0);
        lv_obj_set_style_text_color(bubble, lv_color_hex(0xE0F7FA), 0);
    }

    lv_obj_t *label = lv_label_create(bubble);
    lv_label_set_text(label, d->text);
    lv_obj_set_style_text_font(label, &font_cjk_16, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, 175);

    g_chat_msg_count++;
    lv_obj_scroll_to_view_recursive(bubble, LV_ANIM_OFF);
    free(d);
}

// 聊天Tab创建
static void create_chat_tab(lv_obj_t *parent) {
    lv_obj_set_style_pad_all(parent, 4, 0);

    // ---- 左侧：快捷指令按钮（~100px） ----
    lv_obj_t *left_panel = lv_obj_create(parent);
    lv_obj_set_size(left_panel, 100, 220);
    lv_obj_align(left_panel, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_pad_all(left_panel, 4, 0);
    lv_obj_set_style_radius(left_panel, 8, 0);
    lv_obj_set_style_bg_color(left_panel, lv_color_hex(0x16213e), 0);
    lv_obj_set_style_border_width(left_panel, 0, 0);
    lv_obj_clear_flag(left_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_title = lv_label_create(left_panel);
    lv_label_set_text(btn_title, "快捷指令");
    lv_obj_set_style_text_font(btn_title, &font_cjk_16, 0);
    lv_obj_set_style_text_color(btn_title, lv_color_hex(0xB0BEC5), 0);
    lv_obj_align(btn_title, LV_ALIGN_TOP_MID, 0, 2);

    // 纵向排列按钮
    lv_obj_t *btn_col = lv_obj_create(left_panel);
    lv_obj_set_size(btn_col, 90, 192);
    lv_obj_align(btn_col, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(btn_col, 2, 0);
    lv_obj_set_style_bg_opa(btn_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_col, 0, 0);
    lv_obj_set_flex_flow(btn_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btn_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_col, LV_OBJ_FLAG_SCROLLABLE);

    // ---- 右侧：对话框（~210px） ----
    lv_obj_t *right_panel = lv_obj_create(parent);
    lv_obj_set_size(right_panel, 210, 220);
    lv_obj_align(right_panel, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_pad_all(right_panel, 4, 0);
    lv_obj_set_style_radius(right_panel, 8, 0);
    lv_obj_set_style_bg_color(right_panel, lv_color_hex(0x263238), 0);
    lv_obj_set_style_border_width(right_panel, 0, 0);

    lv_obj_t *reply_title = lv_label_create(right_panel);
    lv_label_set_text(reply_title, "AI 回复");
    lv_obj_set_style_text_font(reply_title, &font_cjk_16, 0);
    lv_obj_set_style_text_color(reply_title, lv_color_hex(0x80CBC4), 0);
    lv_obj_align(reply_title, LV_ALIGN_TOP_LEFT, 2, 2);

    // 消息滚动区
    chat_msg_area = lv_obj_create(right_panel);
    lv_obj_set_size(chat_msg_area, 200, 192);
    lv_obj_align(chat_msg_area, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(chat_msg_area, 4, 0);
    lv_obj_set_style_bg_color(chat_msg_area, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(chat_msg_area, 0, 0);
    lv_obj_set_flex_flow(chat_msg_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(chat_msg_area, 3, 0);
    lv_obj_set_scrollbar_mode(chat_msg_area, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(chat_msg_area, LV_OBJ_FLAG_SCROLLABLE);

    static const char *short_labels[] = {
        "你好小智", "查天气", "问时间", "查温度",
    };

    for (int i = 0; i < PRESET_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(btn_col);
        lv_obj_set_size(btn, 88, 42);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x37474F), 0);
        lv_obj_set_style_shadow_width(btn, 4, 0);
        lv_obj_set_style_shadow_ofs_y(btn, 2, 0);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, short_labels[i]);
        lv_obj_set_style_text_font(lbl, &font_cjk_16, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, chat_preset_btn_cb, LV_EVENT_CLICKED,
                           (void *)preset_sentences[i]);
    }
}

// ---- 公开 API ----

void ui_chat_set_send_callback(ui_chat_send_cb_t cb) {
    g_chat_send_cb = cb;
}

void ui_chat_add_message(const char *text, bool is_user) {
    if (!text || !chat_msg_area) return;
    chat_msg_async_t *d = malloc(sizeof(chat_msg_async_t));
    if (!d) return;
    strncpy(d->text, text, sizeof(d->text) - 1);
    d->text[sizeof(d->text) - 1] = '\0';
    d->is_user = is_user;
    if (lv_async_call(chat_add_msg_async, d) != LV_RES_OK) free(d);
}

void ui_chat_set_enabled(bool enabled) {
    // reserved for future use
}
