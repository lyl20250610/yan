#include "sgp30.h"
#include "esp_check.h"
#include "esp_err.h"
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "SGP30";
#define SGP30_I2C_ADDR 0x58
#define CMD_INIT_AIR_QUALITY 0x2003
#define CMD_MEASURE_AIR_QUALITY 0x2008

static uint8_t crc8(uint8_t *data, size_t len) {
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x31;
            else crc <<= 1;
        }
    }
    return crc;
}

struct sgp30_t {
    i2c_port_t i2c_num;
};

sgp30_handle_t sgp30_create(i2c_port_t i2c_num) {
    sgp30_handle_t handle = calloc(1, sizeof(struct sgp30_t));
    if (handle) handle->i2c_num = i2c_num;
    return handle;
}

void sgp30_delete(sgp30_handle_t handle) {
    free(handle);
}

esp_err_t sgp30_init(sgp30_handle_t handle) {
    esp_err_t ret;
    uint8_t cmd[2] = { CMD_INIT_AIR_QUALITY >> 8, CMD_INIT_AIR_QUALITY & 0xFF };

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            vTaskDelay(pdMS_TO_TICKS(200));
            ESP_LOGW(TAG, "Retry init (attempt %d)", attempt + 1);
        }
        ret = i2c_master_write_to_device(handle->i2c_num, SGP30_I2C_ADDR, cmd, 2, pdMS_TO_TICKS(200));
        if (ret == ESP_OK) break;
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Init cmd fail (addr=0x%02x): %s", SGP30_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    vTaskDelay(pdMS_TO_TICKS(30));
    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;
}

esp_err_t sgp30_read_air_quality(sgp30_handle_t handle, uint16_t *tvoc, uint16_t *co2) {
    uint8_t cmd[2] = { CMD_MEASURE_AIR_QUALITY >> 8, CMD_MEASURE_AIR_QUALITY & 0xFF };
    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(handle->i2c_num, SGP30_I2C_ADDR, cmd, 2, pdMS_TO_TICKS(100)), TAG, "Measure cmd fail");
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t data[6];
    ESP_RETURN_ON_ERROR(i2c_master_read_from_device(handle->i2c_num, SGP30_I2C_ADDR, data, 6, pdMS_TO_TICKS(100)), TAG, "Read fail");

    if (crc8(data, 2) != data[2] || crc8(data+3, 2) != data[5]) {
        ESP_LOGE(TAG, "CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    *co2 = (data[0] << 8) | data[1];
    *tvoc = (data[3] << 8) | data[4];
    ESP_LOGI(TAG, "CO2=%u ppm TVOC=%u ppb", *co2, *tvoc);
    return ESP_OK;
}
// 全局变量，保存最后一次读取的传感器数据
static uint16_t g_last_co2 = 400;
static uint16_t g_last_tvoc = 0;

// 更新SGP30传感器数据
esp_err_t sgp30_update(sgp30_handle_t handle) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t co2, tvoc;
    esp_err_t err = sgp30_read_air_quality(handle, &tvoc, &co2);

    if (err == ESP_OK) {
        g_last_co2 = co2;
        g_last_tvoc = tvoc;
    }

    return err;
}

// 获取最后一次读取的TVOC值
uint16_t sgp30_get_tvoc(void) {
    return g_last_tvoc;
}

// 获取最后一次读取的CO2值
uint16_t sgp30_get_co2(void) {
    return g_last_co2;
}
// 获取SGP30传感器的基线值
esp_err_t sgp30_get_baseline(sgp30_handle_t handle, uint16_t *co2_baseline, uint16_t *tvoc_baseline) {
    if (!handle || !co2_baseline || !tvoc_baseline) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[2] = {0x20, 0x15}; // SGP30获取基线命令
    uint8_t data[6];

    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(handle->i2c_num, SGP30_I2C_ADDR, cmd, 2, pdMS_TO_TICKS(100)), TAG, "Get baseline cmd fail");
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(i2c_master_read_from_device(handle->i2c_num, SGP30_I2C_ADDR, data, 6, pdMS_TO_TICKS(100)), TAG, "Read baseline fail");

    if (crc8(data, 2) != data[2] || crc8(data+3, 2) != data[5]) {
        ESP_LOGE(TAG, "Baseline CRC error");
        return ESP_ERR_INVALID_CRC;
    }

    *co2_baseline = (data[0] << 8) | data[1];
    *tvoc_baseline = (data[3] << 8) | data[4];

    ESP_LOGI(TAG, "Got baseline: CO2=0x%04X TVOC=0x%04X", *co2_baseline, *tvoc_baseline);
    return ESP_OK;
}

// 恢复SGP30传感器的基线值
esp_err_t sgp30_restore_baseline(sgp30_handle_t handle, uint16_t co2_baseline, uint16_t tvoc_baseline) {
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t cmd[8];
    cmd[0] = 0x20; // SGP30设置基线命令
    cmd[1] = 0x1e;

    // TVOC基线在前，CO2基线在后（注意顺序！）
    cmd[2] = (tvoc_baseline >> 8) & 0xFF;
    cmd[3] = tvoc_baseline & 0xFF;
    cmd[4] = crc8(cmd+2, 2);

    cmd[5] = (co2_baseline >> 8) & 0xFF;
    cmd[6] = co2_baseline & 0xFF;
    cmd[7] = crc8(cmd+5, 2);

    ESP_RETURN_ON_ERROR(i2c_master_write_to_device(handle->i2c_num, SGP30_I2C_ADDR, cmd, 8, pdMS_TO_TICKS(100)), TAG, "Restore baseline cmd fail");

    ESP_LOGI(TAG, "Restored baseline: CO2=0x%04X TVOC=0x%04X", co2_baseline, tvoc_baseline);
    return ESP_OK;
}
