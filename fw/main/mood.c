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

stat_t              g_stat = {0};
int16_t             g_sig_hist[SIG_HIST_N] = {0};
int16_t             g_sinr_hist[SIG_HIST_N] = {0};
int                 g_sig_head = 0, g_sig_cnt = 0;
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
//   吞吐轴 = 忙不忙（迟滞：进 >250k / 退 <120k）
//   信号轴 = 舒不舒服（grin / happy / 蔫），三档各留死区迟滞；模组瞬时无效读数沿用上一拍
static TickType_t s_motion_t = 0;    // 最近一次 IMU 运动时间戳
void mood_note_motion(void) { s_motion_t = xTaskGetTickCount(); }

static bool s_busy = false;          // 吞吐忙碌迟滞态
static int  s_sig  = M_HAPPY;        // 信号档迟滞态（grin/happy/wilt）
static long s_rsrp = -90, s_sinr = 10;  // 上一拍有效信号（滤无效用）

static int map_mood(cJSON *j)
{
    if (jnum(j, "online", 1) == 0) return M_OFFLINE;          // 断网（最高优先）
    long dl = jnum(j, "dl_bps", 0), ul = jnum(j, "ul_bps", 0);
    long thr = dl + ul;

    // 滤无效：rsrp 必须落在合理范围(-150..-30)，否则沿用上一拍（模组会瞬时报 0/异常）
    long rr = jnum(j, "rsrp", 0), ss = jnum(j, "sinr", 99);
    if (rr <= -30 && rr >= -150) { s_rsrp = rr; s_sinr = ss; }
    long rsrp = s_rsrp, sinr = s_sinr;

    // 推进 RSRP/SINR 历史（趋势图用）
    g_sig_hist[g_sig_head]  = (int16_t)rsrp;
    g_sinr_hist[g_sig_head] = (int16_t)sinr;
    g_sig_head = (g_sig_head + 1) % SIG_HIST_N;
    if (g_sig_cnt < SIG_HIST_N) g_sig_cnt++;

    // 吞吐轴：忙碌迟滞
    if (s_busy) { if (thr < 120000) s_busy = false; }
    else        { if (thr > 250000) s_busy = true;  }
    if (s_busy) return M_BUSY;

    // 信号轴：三档迟滞，边界各留死区，临界不闪
    switch (s_sig) {
        case M_GRIN: if (rsrp < -95 && sinr < 10) s_sig = M_HAPPY; break;   // 跌出美滋滋
        case M_WILT: if (rsrp > -100 || sinr > 8) s_sig = M_HAPPY; break;   // 缓过来
        default:     if (rsrp >= -90 || sinr >= 12)      s_sig = M_GRIN;    // 升到美滋滋
                     else if (rsrp < -107 && sinr < 5)   s_sig = M_WILT;    // 跌到蔫
                     break;
    }
    return s_sig;
}

void mood_update(cJSON *j)
{
    long dl = jnum(j, "dl_bps", 0), ul = jnum(j, "ul_bps", 0);
    long thr = dl + ul;
    int  online = (int)jnum(j, "online", 1);
    int  bandc  = (int)jnum(j, "band_count", 0);
    cJSON *jm = cJSON_GetObjectItem(j, "mode");
    const char *mode = (jm && cJSON_IsString(jm)) ? jm->valuestring : "";

    // 存一份原始数值供数值页显示
    cJSON *jb = cJSON_GetObjectItem(j, "band");
    const char *band = (jb && cJSON_IsString(jb)) ? jb->valuestring : "";
    g_stat.online = online; g_stat.band_count = bandc;
    g_stat.rsrp = (int)jnum(j, "rsrp", 0);  g_stat.sinr = (int)jnum(j, "sinr", 0);
    g_stat.temp = (int)jnum(j, "temp", 0);  g_stat.dl = dl;  g_stat.ul = ul;
    strncpy(g_stat.mode, mode, sizeof g_stat.mode - 1); g_stat.mode[sizeof g_stat.mode - 1] = 0;
    strncpy(g_stat.band, band, sizeof g_stat.band - 1); g_stat.band[sizeof g_stat.band - 1] = 0;

    // 设备列表：在线设备数 + 新上线主机名（路由器算好）
    int clients = (int)jnum(j, "clients", 0);
    cJSON *jnd = cJSON_GetObjectItem(j, "newdev");
    const char *newdev = (jnd && cJSON_IsString(jnd)) ? jnd->valuestring : "";
    g_stat.clients = clients;

    int target = map_mood(j);

    // E4 久闲→sleepy：信号好(grin/happy) + 吞吐持续很低 ~5min
    static int idle_polls = 0;
    if (g_touched || (xTaskGetTickCount() - s_motion_t) < pdMS_TO_TICKS(8000)) idle_polls = 0;
    else if ((target == M_HAPPY || target == M_GRIN) && thr < 150000) {   // 没流量门槛 150k/s（后台/轻量也算闲）
        if (idle_polls < 9999) idle_polls++;
    } else idle_polls = 0;
    if (idle_polls >= 75) target = M_SLEEPY;   // 75×4s ≈ 5min 打盹

    // 防抖：新 mood 连续 2 次轮询稳定才切换（offline 立即生效）
    static int cand = M_HAPPY, cand_cnt = 0;
    if (target == M_OFFLINE || target == g_mood) {
        g_mood = target; cand_cnt = 0;
    } else if (target == cand) {
        if (++cand_cnt >= 2) { g_mood = target; cand_cnt = 0; }
    } else { cand = target; cand_cnt = 1; }

    // E2 事件检测（与上一拍比）→ 临时盖 surprised + 短气泡 4.5s
    static int  prev_init = 0, prev_online = 1, prev_bandc = -1, prev_clients = -1;
    static char prev_mode[24] = "";
    if (prev_init) {
        int fired = 0;
        if (prev_clients >= 0 && clients > prev_clients) {              // 新设备上线 → 是新朋友！
            if (newdev[0] && newdev[0] != '*') snprintf(g_evt_bub, sizeof g_evt_bub, "新朋友 %s!", newdev);
            else strcpy(g_evt_bub, "是新朋友！");
            fired = 1;
        } else if (online && !prev_online) {
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
    prev_online = online; prev_bandc = bandc; prev_clients = clients;
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
