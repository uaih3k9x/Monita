// 显示层：CO5300 面板 + 分块推屏 + SDF 画眼 + 字库 + 气泡/电量/数值页
// 拥有面板句柄、PSRAM 缓冲、几何常量；对外只暴露高层"画什么"的接口。
#pragma once
#include <stdbool.h>
#include "mood.h"               // mood_t

#define LCD_W 466
#define LCD_H 466               // 屏幕尺寸（屏中心 x = LCD_W/2）

bool display_init(void);        // 面板 + 分配缓冲 + 清屏；缓冲失败返回 false
void display_clear(void);
void display_face(const mood_t *m, int t, float by, float gx, float openK);
void display_bubble(const char *s);              // 空串=清气泡带
void display_battery(int pct, bool charging);    // pct<0=清除
void display_stats(void);                        // 数值页（标题含版本号 + 蜂窝指标）
