#include "lvgl_port.h"
#include "display.h"
#include "touch.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_ops.h"

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = NULL;
static lv_color_t *buf2 = NULL;
static int flush_count = 0;

static void disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_map)
{
    flush_count++;
    if (flush_count <= 5 || flush_count % 100 == 0) {
        size_t data_size = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1) * sizeof(lv_color_t);
        ESP_LOGI("LVGL", "Flush #%d: (%d,%d)-(%d,%d) size=%d ptr=%p dma=%d",
                 flush_count, area->x1, area->y1, area->x2, area->y2,
                 (int)data_size, (void*)color_map, esp_ptr_dma_capable(color_map));
    }

    esp_lcd_panel_handle_t panel = display_get_panel();
    if (panel) {
        esp_err_t err = esp_lcd_panel_draw_bitmap(panel,
                                                  area->x1, area->y1,
                                                  area->x2 + 1, area->y2 + 1,
                                                  (void *)color_map);
        if (err == ESP_OK) {
            display_wait_for_flush(200);
        } else {
            ESP_LOGW("LVGL", "draw_bitmap failed: %s (flush #%d)", esp_err_to_name(err), flush_count);
        }
    }
    lv_disp_flush_ready(disp_drv);
}

static void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    touch_point_t tp;
    if (touch_read(&tp) && tp.pressed) {
        data->point.x = tp.x;
        data->point.y = tp.y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static void lv_tick_cb(void *arg) {
    lv_tick_inc(5);
}

esp_err_t lvgl_port_init(void)
{
    lv_init();

    const esp_timer_create_args_t tick_args = {
        .callback = &lv_tick_cb,
        .name = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    esp_err_t err = esp_timer_create(&tick_args, &tick_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(tick_timer, 5000);
    if (err != ESP_OK) {
        esp_timer_delete(tick_timer);
        return err;
    }
    ESP_LOGI("LVGL", "Tick timer started");

    size_t buf_size = DISPLAY_WIDTH * 15 * sizeof(lv_color_t);  // 15 lines for fewer flushes = smoother
    buf1 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    buf2 = heap_caps_aligned_alloc(64, buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (!buf1 || !buf2) {
        ESP_LOGE("LVGL", "FATAL: Cannot allocate draw buffers!");
        if (buf1) { heap_caps_free(buf1); buf1 = NULL; }
        if (buf2) { heap_caps_free(buf2); buf2 = NULL; }
        esp_timer_stop(tick_timer);
        esp_timer_delete(tick_timer);
        return ESP_ERR_NO_MEM;
    } else {
        ESP_LOGI("LVGL", "Draw buffers OK: %d bytes each", (int)buf_size);
        ESP_LOGI("LVGL", "buf1=%p dma_cap=%d", (void*)buf1, esp_ptr_dma_capable(buf1));
        ESP_LOGI("LVGL", "buf2=%p dma_cap=%d", (void*)buf2, esp_ptr_dma_capable(buf2));
    }


    ESP_LOGI("LVGL", "DMA heap free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI("LVGL", "Internal DMA heap free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    ESP_LOGI("LVGL", "Internal heap free: %d bytes",
             (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size / sizeof(lv_color_t));

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DISPLAY_WIDTH;
    disp_drv.ver_res = DISPLAY_HEIGHT;
    disp_drv.flush_cb = disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = touchpad_read;
    lv_indev_drv_register(&indev_drv);
    return ESP_OK;
}

void lvgl_port_task(void *arg)
{
    ESP_LOGI("LVGL", "Task started, entering lv_timer_handler loop");
    int loop_count = 0;
    while (1) {
        uint32_t delay_ms = lv_timer_handler();
        loop_count++;
        if (loop_count <= 3) {
            ESP_LOGI("LVGL", "Loop #%d: lv_timer_handler returned %lu ms",
                     loop_count, (unsigned long)delay_ms);
        }
        if (delay_ms < 15) delay_ms = 15;  // 保证至少 1 tick 的阻塞休眠
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}
