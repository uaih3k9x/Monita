// 电源管理（AXP2101，挂在共享 I2C 总线）—— 读电池百分比 / 充电态
#pragma once
#include <stdbool.h>

extern volatile int  g_batt;       // 电池百分比（-1=未知）
extern volatile bool g_charging;   // 是否在充电

void power_init(void);             // 挂 AXP2101（需先 i2c_bus_init）
void power_read(void);             // 刷新 g_batt / g_charging
