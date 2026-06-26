#include "power.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define AXP_ADDR 0x34

static const char *TAG = "power";

volatile int  g_batt = -1;
volatile bool g_charging = false;
static i2c_master_dev_handle_t s_axp;

static esp_err_t axp_rd(uint8_t reg, uint8_t *val)
{
    return s_axp ? i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100) : ESP_FAIL;
}

void power_init(void)
{
    if (!i2c_bus()) { ESP_LOGW(TAG, "I2C 总线未就绪"); return; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(i2c_bus(), &dc, &s_axp) != ESP_OK) { ESP_LOGW(TAG, "AXP2101 挂载失败"); s_axp = NULL; return; }
    uint8_t id = 0, s0 = 0, s1 = 0, pct = 0xFF;
    axp_rd(0x03, &id); axp_rd(0x00, &s0); axp_rd(0x01, &s1); axp_rd(0xA4, &pct);
    ESP_LOGI(TAG, "AXP2101 id=0x%02x status0=0x%02x status1=0x%02x batt(0xA4)=%d", id, s0, s1, pct);
}

void power_read(void)
{
    uint8_t pct = 0xFF, st = 0;
    if (axp_rd(0xA4, &pct) == ESP_OK && pct <= 100) g_batt = pct;
    if (axp_rd(0x00, &st) == ESP_OK) g_charging = (st & 0x20) != 0;   // bit5 ≈ VBUS/充电
}
