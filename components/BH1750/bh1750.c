#include "esp_check.h"
#include "bh1750.h"
#include "esp_log.h"


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "BH1750";
static i2c_port_t bh_i2c_num;
static uint8_t bh_addr;

#define BH1750_CMD_POWER_ON   0x01
#define BH1750_CMD_RESET      0x07
#define BH1750_CMD_CONT_H_RES 0x10


#define FILTER_WINDOW_SIZE 5
static float lux_filter_buf[FILTER_WINDOW_SIZE] = {0};
static uint8_t filter_idx = 0;
static uint8_t filter_count = 0;   // 已填入的有效样本数，避免启动阶段被缓冲区的 0 拉低读数


static float g_lux = 0.0f;
static SemaphoreHandle_t data_mutex = NULL;


static float sliding_filter(float *buf, float new_val) {
    buf[filter_idx] = new_val;
    if (filter_count < FILTER_WINDOW_SIZE) {
        filter_count++;
    }
    float sum = 0.0f;
    for (int i = 0; i < filter_count; i++) {
        sum += buf[i];
    }
    return sum / filter_count;
}


esp_err_t bh1750_init(i2c_port_t i2c_num, uint8_t addr) {
    bh_i2c_num = i2c_num;
    bh_addr = addr;

    esp_err_t ret;
    uint8_t cmd = BH1750_CMD_POWER_ON;

    // 重试 3 次（部分模块上电需要时间稳定）
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGW(TAG, "Retry power on (attempt %d)", attempt + 1);
        }
        ret = i2c_master_write_to_device(i2c_num, addr, &cmd, 1, pdMS_TO_TICKS(200));
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Power on fail (addr=0x%02x): %s", addr, esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    cmd = BH1750_CMD_RESET;
    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(i2c_num, addr, &cmd, 1, pdMS_TO_TICKS(100)), TAG, "Reset fail");
    vTaskDelay(pdMS_TO_TICKS(10));

    cmd = BH1750_CMD_CONT_H_RES;
    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(i2c_num, addr, &cmd, 1, pdMS_TO_TICKS(100)), TAG, "Set mode fail");
    vTaskDelay(pdMS_TO_TICKS(180));

   
    if (data_mutex == NULL) {
        data_mutex = xSemaphoreCreateMutex();
        if (data_mutex == NULL) {
            ESP_LOGE(TAG, "Failed to create mutex");
            return ESP_ERR_NO_MEM;
        }
    }
    
    ESP_LOGI(TAG, "Initialized, addr=0x%02x", addr);
    return ESP_OK;
}


esp_err_t bh1750_read_lux(float *lux) {
    uint8_t data[2];
    esp_err_t ret;

    // 重试 3 次，避免单次 I2C 抖动导致读取失败
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        ret = i2c_master_read_from_device(bh_i2c_num, bh_addr, data, 2, pdMS_TO_TICKS(100));
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Read fail after 3 retries");
        return ret;
    }
    uint16_t raw = (data[0] << 8) | data[1];
    *lux = raw / 1.2f;
    ESP_LOGI(TAG, "Lux = %.1f", *lux);
    return ESP_OK;
}


float bh1750_get_lux(void) {
    float lux = 0.0f;

    if (data_mutex == NULL) {
        return 0.0f;
    }
    if (xSemaphoreTake(data_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        lux = g_lux;
        xSemaphoreGive(data_mutex);
    }
    return lux;
}


esp_err_t bh1750_update(void)
{
    if (data_mutex == NULL) {
        data_mutex = xSemaphoreCreateMutex();
        if (data_mutex == NULL) return ESP_ERR_NO_MEM;
    }

    float lux = 0.0f;
    esp_err_t ret = bh1750_read_lux(&lux);
    if (ret == ESP_OK) {
        filter_idx = (filter_idx + 1) % FILTER_WINDOW_SIZE;
        float filt_lux = sliding_filter(lux_filter_buf, lux);
        if (xSemaphoreTake(data_mutex, portMAX_DELAY) == pdTRUE) {
            g_lux = filt_lux;
            xSemaphoreGive(data_mutex);
        }
    }
    return ret;
}

esp_err_t bh1750_reinit(void)
{
    uint8_t cmd;
    esp_err_t ret;

    ESP_LOGI(TAG, "Attempting reinit (addr=0x%02x)", bh_addr);

    // 重新上电
    cmd = BH1750_CMD_POWER_ON;
    ret = i2c_master_write_to_device(bh_i2c_num, bh_addr, &cmd, 1, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Reinit: power on fail");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 复位
    cmd = BH1750_CMD_RESET;
    ret = i2c_master_write_to_device(bh_i2c_num, bh_addr, &cmd, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Reinit: reset fail");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    // 设置连续高分辨率模式
    cmd = BH1750_CMD_CONT_H_RES;
    ret = i2c_master_write_to_device(bh_i2c_num, bh_addr, &cmd, 1, pdMS_TO_TICKS(100));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Reinit: set mode fail");
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(180));

    // 清空滑动滤波缓存，避免旧值干扰
    for (int i = 0; i < FILTER_WINDOW_SIZE; i++) {
        lux_filter_buf[i] = 0.0f;
    }
    filter_idx = 0;

    ESP_LOGI(TAG, "Reinit success");
    return ESP_OK;
}

