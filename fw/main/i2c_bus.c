#include "i2c_bus.h"
#include "esp_log.h"

#define I2C_SDA 15
#define I2C_SCL 14

static const char *TAG = "i2c";
static i2c_master_bus_handle_t s_bus;

esp_err_t i2c_bus_init(void)
{
    if (s_bus) return ESP_OK;
    i2c_master_bus_config_t bc = {
        .i2c_port = -1, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t e = i2c_new_master_bus(&bc, &s_bus);
    if (e != ESP_OK) { ESP_LOGW(TAG, "I2C 建失败"); s_bus = NULL; }
    return e;
}

i2c_master_bus_handle_t i2c_bus(void) { return s_bus; }
