// LVGL 页面（lvgl-settings 分支）：仪表盘(page1) + 设置页(page3)，与裸渲染分时复用面板。
#pragma once
#include <stdbool.h>

extern volatile int  g_user_bright;     // 亮度（滑块 ↔ 面板共享）
extern volatile bool g_badge_refresh;   // 按钮请求清吧唧缓存
extern volatile bool g_lvgl_exit;       // Back 按钮请求回脸

void lvgl_init(void);          // 初始化 LVGL + 显示/触摸 + 构建仪表盘/设置两屏（开机一次）
void lvgl_show_dash(void);     // 切到仪表盘屏并标脏
void lvgl_show_settings(void); // 切到设置屏并标脏
void lvgl_tick(void);          // 在 LVGL 页每帧调：lv_timer_handler
