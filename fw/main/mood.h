// 情绪模型 + 两轴映射 + 事件状态机（Phase E 的逻辑都在这）
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"   // TickType_t
#include "cJSON.h"

// 眼形
typedef enum { EYE_ROUND, EYE_WIDE, EYE_HAPPY, EYE_SLEEPY, EYE_STRAIN, EYE_SQUEEZE } eye_shape_t;

typedef struct {
    const char  *name;
    eye_shape_t  shape;
    float        hw, hh;     // 眼半宽 / 半高
    float        glow;       // 光晕扩散
    float        breathe;    // 呼吸速率倍数
    bool         blink;      // 是否眨眼（仅对填充眼形有意义）
    bool         shake;      // 抖（offline）
    bool         blush;      // 脸红（眼下淡红）
    bool         sweat;      // 汗滴（busy，右侧滑落）
    bool         tear;       // 蓝泪（offline，眼下滚落）
    const char  *bubble;     // 台词气泡（空=不显示）
} mood_t;

enum { M_HAPPY, M_GRIN, M_BUSY, M_SURPRISED, M_OFFLINE, M_SLEEPY, M_WILT };  // 与 MOODS 顺序一致
#define N_MOODS 7
extern const mood_t MOODS[];

// 最近一拍 face.json 的原始数值（数值页显示用）
typedef struct {
    int  online, rsrp, sinr, band_count, temp;
    long dl, ul;
    char mode[24];
    char band[12];
} stat_t;
extern stat_t g_stat;

// RSRP 历史（数值页趋势图用）：环形缓冲，每次有效 poll 推一个
#define SIG_HIST_N 120
extern int16_t g_sig_hist[SIG_HIST_N];
extern int     g_sig_head, g_sig_cnt;

// 共享状态（渲染循环读，mood_update 写）
extern volatile int        g_mood;        // 目标表情
extern char                g_dyn_bub[64]; // 稳态动态气泡：busy 实时吞吐 / offline / sleepy 性格台词
extern char                g_evt_bub[64]; // 事件气泡（载波/制式/上线）
extern volatile int        g_evt_mood;    // 事件期间盖的表情
extern volatile TickType_t g_evt_until;   // 事件覆盖到期 tick（0=从未触发）

// 用一拍 face.json 更新 g_mood + 事件 + 动态气泡（含迟滞/防抖/久闲）
void mood_update(cJSON *j);
