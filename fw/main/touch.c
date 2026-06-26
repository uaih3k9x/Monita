#include "touch.h"
#include "i2c_bus.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define TP_ADDR  0x5A
#define TP_RST   40
#define TP_INT   11

static const char *TAG = "touch";
static i2c_master_dev_handle_t s_tp;

volatile bool g_touched = false;
volatile int  g_tx = 0, g_ty = 0;

static esp_err_t tp_rd(uint16_t reg, uint8_t *data, size_t len)
{
    if (!s_tp) return ESP_FAIL;
    uint8_t rb[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (i2c_master_transmit(s_tp, rb, 2, 100) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_receive(s_tp, data, len, 100);
}

// 写 16 位寄存器：先写地址，再写值（两段事务，与官方驱动一致）
static esp_err_t tp_wr(uint16_t reg, const uint8_t *val, size_t vlen)
{
    if (!s_tp) return ESP_FAIL;
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (i2c_master_transmit(s_tp, a, 2, 100) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_transmit(s_tp, val, vlen, 100);
}

void touch_init(void)
{
    if (!i2c_bus()) return;
    gpio_config_t rc = { .pin_bit_mask = BIT64(TP_RST), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    gpio_config_t ic = { .pin_bit_mask = BIT64(TP_INT), .mode = GPIO_MODE_INPUT };
    gpio_config(&ic);
    gpio_set_level(TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(i2c_bus(), &dc, &s_tp) != ESP_OK) { s_tp = NULL; ESP_LOGW(TAG, "触摸挂载失败"); return; }

    // 初始化握手（照官方驱动 read_config）：进命令模式 + 读校验码/分辨率
    uint8_t cm[2] = { 0xD1, 0x01 };
    tp_wr(0xD101, cm, 2);  vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t cc[4] = {0}, rs[4] = {0};
    tp_rd(0xD1FC, cc, 4);  // checkcode
    tp_rd(0xD1F8, rs, 4);  // resolution
    ESP_LOGI(TAG, "CST9217 checkcode=%02X%02X%02X%02X 分辨率=%dx%d",
             cc[0], cc[1], cc[2], cc[3], (rs[1] << 8) | rs[0], (rs[3] << 8) | rs[2]);

    uint8_t d[10] = {0};
    esp_err_t r = tp_rd(0xD000, d, sizeof(d));     // 自检：ack 应为 0xAB
    ESP_LOGI(TAG, "CST9217 自检 rd=%s ack=0x%02X points=%d", r == ESP_OK ? "OK" : "FAIL", d[6], d[5] & 0x7F);
}

static bool tp_read(int *x, int *y)
{
    uint8_t d[10] = {0};
    if (tp_rd(0xD000, d, sizeof(d)) != ESP_OK) return false;
    if (d[6] != 0xAB) return false;
    if ((d[5] & 0x7F) < 1) return false;
    if ((d[0] & 0x0F) != 0x06) return false;
    *x = (d[1] << 4) | (d[3] >> 4);
    *y = (d[2] << 4) | (d[3] & 0x0F);
    return true;
}

void touch_task(void *arg)
{
    bool was = false; int cnt = 0;
    while (true) {
        int x = 0, y = 0;
        bool t = tp_read(&x, &y);
        if (t) { g_tx = x; g_ty = y; }
        if (t && !was) ESP_LOGI(TAG, "↓ 摸到 x=%d y=%d", x, y);
        else if (!t && was) ESP_LOGI(TAG, "↑ 松手");
        else if (t && (++cnt % 15 == 0)) ESP_LOGI(TAG, "… 摸 x=%d y=%d", x, y);
        g_touched = t; was = t;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
