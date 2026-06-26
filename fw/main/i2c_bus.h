// 共享 I2C 总线（SDA15/SCL14）—— AXP2101 电量 与 CST9217 触摸共用
#pragma once
#include "driver/i2c_master.h"

esp_err_t i2c_bus_init(void);            // 创建总线（幂等）；失败返回非 ESP_OK
i2c_master_bus_handle_t i2c_bus(void);   // 取总线句柄（未初始化返回 NULL）
