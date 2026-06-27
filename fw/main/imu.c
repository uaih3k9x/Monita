#include "imu.h"
#include "i2c_bus.h"
#include "esp_log.h"

#define WHO_AM_I 0x00
#define CTRL1    0x02   // 接口 / 地址自增
#define CTRL2    0x03   // 加速度 ODR + 量程
#define CTRL7    0x08   // 使能各传感器
#define AX_L     0x35   // 加速度数据起始（6 字节，地址自增连读）

static const char *TAG = "imu";
static i2c_master_dev_handle_t s_imu;
bool g_imu_ok = false;

static esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_imu, &reg, 1, buf, n, 100);
}
static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_imu, b, 2, 100);
}

void imu_init(void)
{
    if (!i2c_bus()) return;
    const uint8_t addrs[2] = { 0x6B, 0x6A };
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7,
                                   .device_address = addrs[i], .scl_speed_hz = 400000 };
        if (i2c_master_bus_add_device(i2c_bus(), &dc, &s_imu) != ESP_OK) { s_imu = NULL; continue; }
        uint8_t who = 0;
        if (rd(WHO_AM_I, &who, 1) == ESP_OK && who == 0x05) {
            wr(CTRL1, 0x40);   // 地址自增（连读用）
            wr(CTRL2, 0x06);   // 加速度 ±2g, ODR ~125Hz
            wr(CTRL7, 0x01);   // 只使能加速度
            g_imu_ok = true;
            ESP_LOGI(TAG, "QMI8658 @0x%02X 就绪 (who=0x05)", addrs[i]);
            return;
        }
        i2c_master_bus_rm_device(s_imu); s_imu = NULL;
    }
    ESP_LOGW(TAG, "未找到 QMI8658 IMU");
}

bool imu_read(float *ax, float *ay, float *az)
{
    if (!g_imu_ok) return false;
    uint8_t d[6];
    if (rd(AX_L, d, 6) != ESP_OK) return false;
    int16_t x = (int16_t)(d[0] | (d[1] << 8));
    int16_t y = (int16_t)(d[2] | (d[3] << 8));
    int16_t z = (int16_t)(d[4] | (d[5] << 8));
    const float s = 1.0f / 16384.0f;   // ±2g 灵敏度
    *ax = x * s; *ay = y * s; *az = z * s;
    return true;
}
