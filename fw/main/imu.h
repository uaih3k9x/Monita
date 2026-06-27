// QMI8658 6 轴 IMU（加速度，挂在共享 I2C 总线，地址 0x6B/0x6A）
#pragma once
#include <stdbool.h>

extern bool g_imu_ok;

void imu_init(void);                              // 探测 + 配置（需先 i2c_bus_init）
bool imu_read(float *ax, float *ay, float *az);   // 读加速度(单位 g)，无 IMU/失败返回 false
