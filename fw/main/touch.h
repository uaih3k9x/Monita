// 触摸 CST9217（自写极简驱动，挂在共享 I2C 总线，地址 0x5A，RST40/INT11）
#pragma once
#include <stdbool.h>

extern volatile bool g_touched;     // 当前是否被摸
extern volatile int  g_tx, g_ty;    // 最近触点坐标

void touch_init(void);              // 初始化 CST9217（需先 i2c_bus_init）
void touch_task(void *arg);         // 触摸轮询任务（50Hz），更新 g_touched/g_tx/g_ty
