#include "apds9960.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "APDS9960";
#define APDS9960_ADDR 0x39

/* 寄存器地址（依据 APDS-9960 数据手册与 SparkFun / Adafruit 参考实现）
 * 注意：0x83 是 WTIME（等待时间），并不是手势控制寄存器。 */
#define REG_ENABLE     0x80
#define REG_ATIME      0x81
#define REG_WTIME      0x83
#define REG_PPULSE     0x8E
#define REG_CONFIG2    0x90
#define REG_ID         0x92
#define REG_GPENTH     0xA0
#define REG_GEXTH      0xA1
#define REG_GCONF1     0xA2
#define REG_GCONF2     0xA3
#define REG_GOFFSET_U  0xA4
#define REG_GOFFSET_D  0xA5
#define REG_GPULSE     0xA6
#define REG_GOFFSET_L  0xA7
#define REG_GOFFSET_R  0xA9
#define REG_GCONF3     0xAA
#define REG_GCONF4     0xAB
#define REG_GFLVL      0xAE
#define REG_GSTATUS    0xAF
#define REG_GFIFO_U    0xFC   /* 之后连续 4 字节依次为 U, D, L, R */

/* ENABLE(0x80) 位定义 */
#define ENABLE_PON     0x01
#define ENABLE_PEN     0x04
#define ENABLE_WEN     0x08
#define ENABLE_GEN     0x40   /* 手势引擎总开关，必须置 1 */
#define GSTATUS_GVALID 0x01   /* 手势 FIFO 已有数据 */

/* 手势判定参数 */
#define GESTURE_MIN_SUM   30  /* 单方向累计强度下限，低于此判为"无手势" */
#define GESTURE_FIFO_MAX  32  /* 单次轮询最多排空的 FIFO 数据集个数 */

static i2c_port_t i2c_port;

/* 一次手势过程中的四方向累计强度 */
static uint32_t sum_up, sum_down, sum_left, sum_right;
static bool gesture_in_progress = false;

static esp_err_t write_reg(uint8_t reg, uint8_t val) {
    uint8_t data[2] = {reg, val};
    return i2c_master_write_to_device(i2c_port, APDS9960_ADDR, data, 2, pdMS_TO_TICKS(100));
}

static esp_err_t read_reg(uint8_t reg, uint8_t *val) {
    return i2c_master_write_read_device(i2c_port, APDS9960_ADDR, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

/* 从 GFIFO 起始地址突发读取 len 字节（必须是一次重复起始的写地址+连续读） */
static esp_err_t read_fifo(uint8_t *buf, size_t len) {
    uint8_t start = REG_GFIFO_U;
    return i2c_master_write_read_device(i2c_port, APDS9960_ADDR, &start, 1, buf, len,
                                        pdMS_TO_TICKS(100));
}

static void gesture_reset(void) {
    sum_up = sum_down = sum_left = sum_right = 0;
    gesture_in_progress = false;
}

/* 方向判定：沿用 Adafruit 参考实现的极性
 * （U 主导 → DOWN，D 主导 → UP，L 主导 → RIGHT，R 主导 → LEFT）。
 * 若实测上下 / 左右相反，只需交换下面四个赋值即可。 */
static uint8_t gesture_decide(void) {
    uint32_t max_sum = sum_up;
    uint8_t dir = GESTURE_DOWN;

    if (sum_down > max_sum) { max_sum = sum_down; dir = GESTURE_UP; }
    if (sum_left > max_sum) { max_sum = sum_left; dir = GESTURE_RIGHT; }
    if (sum_right > max_sum) { max_sum = sum_right; dir = GESTURE_LEFT; }

    if (max_sum < GESTURE_MIN_SUM) {
        return 0;   /* 数据量不足，判为无手势，避免误触发翻页 */
    }
    return dir;
}

esp_err_t apds9960_init(i2c_port_t i2c_num) {
    i2c_port = i2c_num;

    /* 芯片 ID 校验：仅告警不失败，兼容国产替代芯片 */
    uint8_t id = 0;
    if (read_reg(REG_ID, &id) == ESP_OK && id != 0xAB) {
        ESP_LOGW(TAG, "Unexpected chip ID 0x%02X (expect 0xAB), continue anyway", id);
    }

    /* 1) 先关电再上电，让寄存器回到已知状态 */
    if (write_reg(REG_ENABLE, 0x00) != ESP_OK) {
        ESP_LOGE(TAG, "Init fail (addr=0x%02x): power down write failed", APDS9960_ADDR);
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    if (write_reg(REG_ENABLE, ENABLE_PON) != ESP_OK) {
        ESP_LOGE(TAG, "Init fail (addr=0x%02x): power on write failed", APDS9960_ADDR);
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 2) 手势引擎参数 */
    write_reg(REG_WTIME, 0xFF);    /* 手势窗口约 710ms（SparkFun 用法） */
    write_reg(REG_ATIME, 0xDB);    /* 积分时间约 103ms */
    write_reg(REG_CONFIG2, 0x01);  /* LED_BOOST = 100% */
    write_reg(REG_GCONF1, 0x40);   /* GFIFOTH=1：FIFO 有一组数据即置 GVALID */
    write_reg(REG_GCONF2, 0x40);   /* GGAIN = 4x, GLDRIVE = 100mA */
    write_reg(REG_GCONF3, 0x00);   /* 四个方向光电二极管全部启用 */
    write_reg(REG_PPULSE, 0x89);   /* 邻近脉冲：16us x 10 */
    write_reg(REG_GPULSE, 0xC9);   /* 手势脉冲：32us x 10 */
    write_reg(REG_GPENTH, 0x28);   /* 进入手势阈值 40 */
    write_reg(REG_GEXTH, 0x1E);    /* 退出手势阈值 30 */
    write_reg(REG_GOFFSET_U, 0x00);
    write_reg(REG_GOFFSET_D, 0x00);
    write_reg(REG_GOFFSET_L, 0x00);
    write_reg(REG_GOFFSET_R, 0x00);

    /* 3) 清 FIFO 后启动手势状态机，最后再置 GEN 打开手势引擎 */
    write_reg(REG_GCONF4, 0x00);   /* GMODE=0, GIEN=0 */
    write_reg(REG_GCONF4, 0x01);   /* GMODE=1 */
    write_reg(REG_ENABLE, ENABLE_PON | ENABLE_WEN | ENABLE_PEN | ENABLE_GEN);
    vTaskDelay(pdMS_TO_TICKS(10));

    /* 4) 丢弃使能过程中产生的无效数据 */
    for (int guard = 0; guard < 8; guard++) {
        uint8_t st = 0;
        if (read_reg(REG_GSTATUS, &st) != ESP_OK || !(st & GSTATUS_GVALID)) {
            break;
        }
        uint8_t level = 0;
        if (read_reg(REG_GFLVL, &level) != ESP_OK) {
            break;
        }
        uint8_t dummy[4];
        for (uint8_t i = 0; i < level; i++) {
            if (read_fifo(dummy, sizeof(dummy)) != ESP_OK) {
                break;
            }
        }
    }

    gesture_reset();
    ESP_LOGI(TAG, "Gesture engine enabled (PON|WEN|PEN|GEN, GMODE=1), polling mode");
    return ESP_OK;
}

esp_err_t apds9960_read_gesture(uint8_t *gesture) {
    if (gesture == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *gesture = 0;

    uint8_t gstatus = 0;
    esp_err_t err = read_reg(REG_GSTATUS, &gstatus);
    if (err != ESP_OK) {
        return err;   /* 读失败直接返回，避免使用未初始化数据 */
    }

    if (!(gstatus & GSTATUS_GVALID)) {
        /* GVALID 落沿说明一次手势结束，此时结算方向 */
        if (gesture_in_progress) {
            uint8_t dir = gesture_decide();
            uint32_t u = sum_up, d = sum_down, l = sum_left, r = sum_right;
            gesture_reset();
            if (dir != 0) {
                *gesture = dir;
                ESP_LOGI(TAG, "Gesture: %u (U=%u D=%u L=%u R=%u)", (unsigned)dir,
                         (unsigned)u, (unsigned)d, (unsigned)l, (unsigned)r);
            }
        }
        return ESP_OK;
    }

    gesture_in_progress = true;

    uint8_t level = 0;
    if (read_reg(REG_GFLVL, &level) != ESP_OK) {
        return ESP_OK;
    }
    if (level > GESTURE_FIFO_MAX) {
        level = GESTURE_FIFO_MAX;   /* 防止异常值导致长时间占用 I2C */
    }

    for (uint8_t i = 0; i < level; i++) {
        uint8_t d[4];
        if (read_fifo(d, sizeof(d)) != ESP_OK) {
            break;
        }
        sum_up += d[0];
        sum_down += d[1];
        sum_left += d[2];
        sum_right += d[3];
    }
    return ESP_OK;
}
