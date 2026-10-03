#include "dht11.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "portmacro.h"

static const char *TAG = "DHT11";
static gpio_num_t dht_pin;

#define FILTER_WINDOW_SIZE 5
static float temp_filter_buf[FILTER_WINDOW_SIZE] = {0};
static float humi_filter_buf[FILTER_WINDOW_SIZE] = {0};
static uint8_t filter_idx = 0;
static uint8_t filter_count = 0;

/* volatile: 读写 float 在 ESP32-S3 上是原子的，不需要互斥锁 */
static volatile float g_temperature = 0.0f;
static volatile float g_humidity = 0.0f;
static volatile TickType_t g_last_valid_tick = 0;

static void delay_us(uint32_t us) {
    uint64_t start = esp_timer_get_time();
    while (esp_timer_get_time() - start < us);
}

static float filter_average(const float *buf) {
    float sum = 0.0f;
    for (int i = 0; i < filter_count; i++) {
        sum += buf[i];
    }
    return filter_count ? sum / filter_count : 0.0f;
}

static void dht11_start(void) {
    gpio_set_direction(dht_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(dht_pin, 0);
    // 起始低电平只要求不少于 18ms，无需在关中断状态下忙等。
    vTaskDelay(pdMS_TO_TICKS(20));
    portDISABLE_INTERRUPTS();
    gpio_set_level(dht_pin, 1);
    delay_us(40);
    gpio_set_direction(dht_pin, GPIO_MODE_INPUT);
}

static uint8_t dht11_read_bit(void) {
    uint64_t start = esp_timer_get_time();
    while (gpio_get_level(dht_pin) == 0) {
        if (esp_timer_get_time() - start > 100)
            return 0;
    }
    start = esp_timer_get_time();
    while (gpio_get_level(dht_pin) == 1) {
        if (esp_timer_get_time() - start > 100)
            return 0;
    }
    uint32_t duration = esp_timer_get_time() - start;
    return (duration > 40) ? 1 : 0;
}

static uint8_t dht11_read_byte(void) {
    uint8_t val = 0;
    for (int i = 0; i < 8; i++) {
        val = (val << 1) | dht11_read_bit();
    }
    return val;
}

esp_err_t dht11_init(gpio_num_t pin) {
    dht_pin = pin;
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << pin),
        .mode = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    return gpio_config(&io_conf);
}

esp_err_t dht11_read(float *temperature, float *humidity) {
    if (!temperature || !humidity)
        return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ESP_OK;
    uint8_t data[5] = {0};

    // dht11_start() 仅在 20ms 起始脉冲结束后关闭中断；后续约 4ms 为时序敏感区。
    dht11_start();

    uint64_t start = esp_timer_get_time();
    while (gpio_get_level(dht_pin) == 1) {
        if (esp_timer_get_time() - start > 100) { ret = ESP_ERR_TIMEOUT; goto exit; }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(dht_pin) == 0) {
        if (esp_timer_get_time() - start > 100) { ret = ESP_ERR_TIMEOUT; goto exit; }
    }
    start = esp_timer_get_time();
    while (gpio_get_level(dht_pin) == 1) {
        if (esp_timer_get_time() - start > 100) { ret = ESP_ERR_TIMEOUT; goto exit; }
    }

    for (int i = 0; i < 5; i++) {
        data[i] = dht11_read_byte();
    }

exit:
    portENABLE_INTERRUPTS();

    if (ret != ESP_OK) return ret;

    if ((data[0] + data[1] + data[2] + data[3]) != data[4]) {
        ESP_LOGE(TAG, "Checksum error");
        return ESP_ERR_INVALID_CRC;
    }

    *humidity = data[0] + data[1] / 10.0f;
    *temperature = data[2] + (data[3] & 0x0F) / 10.0f;
    if (data[2] & 0x80) *temperature = -(*temperature);

    ESP_LOGI(TAG, "T=%.1f°C H=%.1f%%", *temperature, *humidity);
    return ESP_OK;
}

float dht11_get_temperature(void) {
    return g_temperature;
}

float dht11_get_humidity(void) {
    return g_humidity;
}

bool dht11_is_valid(void) {
    TickType_t last = g_last_valid_tick;
    return last != 0 && (xTaskGetTickCount() - last) <= pdMS_TO_TICKS(10000);
}

static void sensor_dht11_task(void *arg) {
    while (1) {
        float temp = 0.0f, humi = 0.0f;
        esp_err_t ret = dht11_read(&temp, &humi);

        if (ret == ESP_OK) {
            // 过滤明显错误的读数
            if (temp >= 0.0f && temp <= 50.0f && humi >= 0.0f && humi <= 100.0f) {
                temp_filter_buf[filter_idx] = temp;
                humi_filter_buf[filter_idx] = humi;
                if (filter_count < FILTER_WINDOW_SIZE) filter_count++;
                g_temperature = filter_average(temp_filter_buf);
                g_humidity = filter_average(humi_filter_buf);
                filter_idx = (filter_idx + 1) % FILTER_WINDOW_SIZE;
                g_last_valid_tick = xTaskGetTickCount();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void dht11_start_task(void) {
    if (xTaskCreate(sensor_dht11_task, "sensor_dht11", 4096, NULL, 1, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create DHT11 task");
    }
}
