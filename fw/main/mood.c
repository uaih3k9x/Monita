#include "mood.h"
#include "touch.h"               // g_touched（久闲计时被摸即清零）
#include "freertos/task.h"       // xTaskGetTickCount / pdMS_TO_TICKS
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "mood";

// 六态 + 蔫（移植自 index.html MOODS，眼形为主）
const mood_t MOODS[] = {
    //          shape        hw  hh  glow brth  blink  shake  blush  sweat  tear   bubble
    {"happy",     EYE_ROUND,   38, 52, 26, 1.0f, true,  false, true,  false, false, ""},
    {"grin",      EYE_HAPPY,   46, 34, 24, 1.0f, false, false, true,  false, false, ""},
    {"busy",      EYE_STRAIN,  38, 28, 16, 1.9f, true,  false, false, true,  false, "clash 满载…"},
    {"surprised", EYE_WIDE,    48, 60, 34, 1.0f, true,  false, true,  false, false, "咦？新设备"},
    {"offline",   EYE_SQUEEZE, 40, 44, 14, 1.0f, false, true,  false, false, true,  "WAN 掉了…"},
    {"sleepy",    EYE_SLEEPY,  42, 30, 12, 0.5f, false, false, false, false, false, "z z z"},
    {"wilt",      EYE_STRAIN,  38, 26, 14, 0.7f, true,  false, false, false, false, ""},
};

volatile int        g_mood = M_HAPPY;
char                g_dyn_bub[64] = "";
char                g_evt_bub[64] = "";
volatile int        g_evt_mood  = M_SURPRISED;
volatile TickType_t g_evt_until = 0;

static long jnum(cJSON *o, const char *k, long def)
{
    cJSON *i = cJSON_GetObjectItem(o, k);
    return (i && cJSON_IsNumber(i)) ? (long)i->valuedouble : def;
}

// face.json → mood：情绪两轴
//   吞吐轴 = 忙不忙（迟滞：进 >250k / 退 <120k，别在边界抖）
//   信号轴 = 舒不舒服（grin / happy / 蔫）
static bool s_busy = false;       // 吞吐忙碌迟滞态（跨轮询保留）

static int map_mood(cJSON *j)
{
    if (jnum(j, "online", 1) == 0) return M_OFFLINE;          // 断网（最高优先）
    long dl = jnum(j, "dl_bps", 0), ul = jnum(j, "ul_bps", 0);
    long sinr = jnum(j, "sinr", 99), rsrp = jnum(j, "rsrp", -50);
    long thr = dl + ul;

    // 吞吐轴：忙碌迟滞
    if (s_busy) { if (thr < 120000) s_busy = false; }        // 退忙
    else        { if (thr > 250000) s_busy = true;  }        // 进忙
    if (s_busy) return M_BUSY;

    // 信号轴：底噪心情
    if (sinr >= 12 || rsrp >= -90) return M_GRIN;            // 信号美滋滋
    if (sinr <  6  && rsrp < -105) return M_WILT;            // 信号差 → 蔫(眯眼无汗)
    return M_HAPPY;                                           // 中间
}

void mood_update(cJSON *j)
{
    long dl = jnum(j, "dl_bps", 0), ul = jnum(j, "ul_bps", 0);
    long thr = dl + ul;
    int  online = (int)jnum(j, "online", 1);
    int  bandc  = (int)jnum(j, "band_count", 0);
    cJSON *jm = cJSON_GetObjectItem(j, "mode");
    const char *mode = (jm && cJSON_IsString(jm)) ? jm->valuestring : "";

    int target = map_mood(j);

    // E4 久闲→sleepy：信号好(grin/happy) + 吞吐持续很低 ~5min
    static int idle_polls = 0;
    if (g_touched) idle_polls = 0;
    else if ((target == M_HAPPY || target == M_GRIN) && thr < 20000) {
        if (idle_polls < 9999) idle_polls++;
    } else idle_polls = 0;
    if (idle_polls >= 75) target = M_SLEEPY;   // 75×4s ≈ 5min

    // 防抖：新 mood 连续 2 次轮询稳定才切换（offline 立即生效）
    static int cand = M_HAPPY, cand_cnt = 0;
    if (target == M_OFFLINE || target == g_mood) {
        g_mood = target; cand_cnt = 0;
    } else if (target == cand) {
        if (++cand_cnt >= 2) { g_mood = target; cand_cnt = 0; }
    } else { cand = target; cand_cnt = 1; }

    // E2 事件检测（与上一拍比）→ 临时盖 surprised + 短气泡 4.5s
    static int  prev_init = 0, prev_online = 1, prev_bandc = -1;
    static char prev_mode[24] = "";
    if (prev_init) {
        int fired = 0;
        if (online && !prev_online) {
            strcpy(g_evt_bub, "上线啦~"); fired = 1;
        } else if (bandc != prev_bandc && bandc > 0 && prev_bandc > 0) {
            snprintf(g_evt_bub, sizeof g_evt_bub, "载波%s%dCC",
                     bandc > prev_bandc ? "↑→" : "↓→", bandc);
            fired = 1;
        } else if (mode[0] && prev_mode[0] && strcmp(mode, prev_mode) != 0) {
            snprintf(g_evt_bub, sizeof g_evt_bub, "切 %s", mode);
            fired = 1;
        }
        if (fired) {
            g_evt_mood  = M_SURPRISED;
            g_evt_until = xTaskGetTickCount() + pdMS_TO_TICKS(4500);
            ESP_LOGI(TAG, "事件→%s", g_evt_bub);
        }
    }
    prev_online = online; prev_bandc = bandc;
    strncpy(prev_mode, mode, sizeof prev_mode - 1);
    prev_mode[sizeof prev_mode - 1] = 0;
    prev_init = 1;

    // E3 稳态动态气泡：busy 显实时吞吐，offline/sleepy 走性格台词，其余空
    if (g_mood == M_BUSY) {
        double mbps = (dl >= ul ? dl : ul) / 1e6;
        snprintf(g_dyn_bub, sizeof g_dyn_bub, "%s%.1fM/s",
                 dl >= ul ? "↓" : "↑", mbps);
    } else if (g_mood == M_OFFLINE) {
        strcpy(g_dyn_bub, "WAN 掉了…");
    } else if (g_mood == M_SLEEPY) {
        strcpy(g_dyn_bub, "z z z");
    } else {
        g_dyn_bub[0] = 0;   // grin/happy/wilt 稳态无气泡
    }
}
