// LVGL 版设置页（lvgl-settings 分支）：脸/数值/媒体仍裸渲染，设置页交给 LVGL，分时复用面板。
#pragma once
#include <stdbool.h>

extern volatile int  g_user_bright;     // 亮度（滑块 ↔ 面板共享）
extern volatile bool g_badge_refresh;   // 按钮请求清吧唧缓存
extern volatile bool g_lvgl_exit;       // Back 按钮请求回脸

void lvgl_init(void);    // 初始化 LVGL + 显示/触摸驱动 + 构建设置界面（开机一次）
void lvgl_enter(void);   // 进设置页：标脏全屏，强制重画
void lvgl_tick(void);    // 在设置页每帧调：lv_timer_handler
