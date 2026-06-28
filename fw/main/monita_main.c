// 模拟太 · Monita 固件（日常昵称「小圆脸」）
// 名字取自《逆转裁判》希月心音的 モニ太——情绪随状态变、会替主人说心里话的胸挂小屏。
// 这里只剩编排：起各模块 + 渲染主循环（动画状态机 + 翻页手势）。细节在各 .c 模块。
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "display.h"             // 显示层：display_init/face/bubble/battery/stats
#include "i2c_bus.h"             // 共享 I2C 总线（电量 + 触摸）
#include "power.h"               // AXP2101 电量（g_batt / g_charging / power_read）
#include "touch.h"               // CST9217 触摸（g_touched / g_tx / g_ty / touch_task）
#include "imu.h"                 // QMI8658 IMU（重力倾斜 → 眼神）
#include "net.h"                 // wifi_start / poll_task / g_net
#include "ota.h"                 // ota_task（OTA 自更新）
#include "mood.h"                // 情绪模型 + MOODS + 共享情绪状态
#include "version.h"             // FW_VERSION
#include "lvgl_ui.h"             // LVGL 设置页（本分支）

static const char *TAG = "monita";

#define BADGE_URL "http://192.168.2.254/badge.m8g"   // 电子吧唧（mkbadge.py 烤好的 .m8g：静态/GIF）
#define MEDIA_TIMEOUT_MS 45000                        // 媒体页停留上限（烧屏保护，自动回脸）
#define BRIGHT_NORMAL 70                              // 常态亮度（省电，原来 100）
#define BRIGHT_SLEEPY 28                              // sleepy 时再暗

static inline float frand(void) { return (float)esp_random() / (float)UINT32_MAX; }

void app_main(void)
{
    ESP_LOGI(TAG, "模拟太 booting");
    if (!display_init()) { ESP_LOGE(TAG, "显示初始化失败"); return; }
    display_brightness(BRIGHT_NORMAL);
    display_bubble(MOODS[M_HAPPY].bubble);

    i2c_bus_init();             // 共享 I2C 总线（电量 + 触摸）
    power_init();               // 电源管理芯片（AXP2101，后台读电量）
    power_read();
    touch_init();               // 触摸（摸摸头）
    imu_init();                 // QMI8658 IMU（重力倾斜）
    lvgl_init();                // LVGL（设置页用，分时复用面板）

    // 联网 + 轮询 face.json + OTA 自更新
    esp_err_t nr = nvs_flash_init();
    if (nr == ESP_ERR_NVS_NO_FREE_PAGES || nr == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    wifi_start();
    xTaskCreate(poll_task,  "poll",  8192, NULL, 4, NULL);
    xTaskCreate(ota_task,   "ota",   8192, NULL, 3, NULL);
    xTaskCreate(touch_task, "touch", 4096, NULL, 5, NULL);   // 触摸读取（高优先级，响应快）
    ESP_LOGI(TAG, "固件版本 v%d", FW_VERSION);

    int   t = 0;
    char  shown_bub[64] = {1, 0};               // 已显示的气泡文本（首帧强制画）
    int   shown_bstate = -99;
    float pet = 0.0f;                          // 摸摸头愉悦度
    int   next_blink = 50; float blink = 0.0f;
    int   next_gaze = 80, gaze_hold = 0; float gx = 0.0f, gaze_target = 0.0f;
    float tilt = 0.0f;                          // 重力倾斜(IMU)→眼神横偏，低通平滑
    bool  moving_prev = false; TickType_t shake_cd = 0;   // 运动检测（唤醒/摇一摇）

    // 翻页 + 手势：快速轻点=翻页，长按≥300ms/滑动=摸头（区分开）
    int   page = 0;                             // 0=脸 1=数值页 2=媒体页(电子吧唧)
    bool  page_dirty = false;
    bool  was_touch = false, moved = false;
    TickType_t down_t = 0; int down_x = 0, down_y = 0;
    TickType_t media_t0 = 0;
    uint8_t *media = NULL; int media_w = 0, media_h = 0, media_nf = 0, media_fi = 0;  // 媒体页(.m8g)播放
    TickType_t media_next = 0;

    // 省电：按需渲染（画面没真变化就不重画）+ 亮度
    const mood_t *last_m = NULL;
    int last_by = 0, last_gx = 0, last_ok = 0, last_sw = -2, last_tr = -2;
    int cur_bright = BRIGHT_NORMAL;

    // 被摸态合成表情（∩ 弯眼 + 强腮红）
    static const mood_t PET_LO = {"pet", EYE_HAPPY, 46, 32, 26, 1.0f, false, false, true, false, false, "好舒服~"};
    static const mood_t PET_HI = {"pet", EYE_HAPPY, 48, 30, 28, 1.0f, false, false, true, false, false, "再摸摸~"};

    while (true) {
        // ── 触摸手势：轻点翻页 vs 长按摸头 ──
        bool tnow = g_touched;
        if (tnow && !was_touch) { down_t = xTaskGetTickCount(); down_x = g_tx; down_y = g_ty; moved = false; }
        if (tnow && (abs(g_tx - down_x) > 40 || abs(g_ty - down_y) > 40)) moved = true;
        if (!tnow && was_touch) {                          // 松手
            bool tap = !moved && (xTaskGetTickCount() - down_t) < pdMS_TO_TICKS(300);
            if (page != 3 && tap) {                         // 设置页交给 LVGL；其余轻点翻页
                page = (page + 1) % 4;                      // 脸→数值→吧唧→设置→脸
                page_dirty = true;
                pet = 0.0f;                                 // 翻页不算摸头
            }
        }
        was_touch = tnow;

        // 摸摸头：只有在脸页、且按住超过 300ms（非轻点）才累积愉悦
        bool holding = tnow && page == 0 && (xTaskGetTickCount() - down_t) >= pdMS_TO_TICKS(300);
        if (holding) pet += 0.06f; else pet -= 0.025f;
        if (pet < 0.0f) pet = 0.0f; else if (pet > 1.3f) pet = 1.3f;
        const bool petting = pet > 0.18f;

        // ── 仪表盘（LVGL）：RSRP/SINR 弧表 + 吞吐曲线 + 信息条 ──
        if (page == 1) {
            if (page_dirty) {
                page_dirty = false;
                display_bubble(""); display_battery(-1, false);
                shown_bub[0] = 1; shown_bub[1] = 0; shown_bstate = -99;
                display_clear();
                lvgl_show_dash();
            }
            lvgl_tick();
            t++;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // ── 媒体页（电子吧唧）：进页拉 .m8g 逐帧循环播；停留超时自动回脸（烧屏保护）──
        if (page == 2) {
            if (page_dirty) {
                page_dirty = false;
                display_bubble(""); display_battery(-1, false);
                shown_bub[0] = 1; shown_bub[1] = 0; shown_bstate = -99;
                media_t0 = xTaskGetTickCount();
                display_clear();
                if (media_nf > 0) {                    // 已缓存 → 秒开，从头播
                    media_fi = 0; media_next = xTaskGetTickCount();
                } else {                               // 首次进页 → 拉取并解析
                    display_message("载入吧唧…");
                    uint8_t *buf = NULL;
                    int n = net_fetch(BADGE_URL, &buf);
                    if (n > 16 && buf[0] == 'M' && buf[1] == '8' && buf[2] == 'G' && buf[3] == '1') {
                        int w = buf[4] | (buf[5] << 8), h = buf[6] | (buf[7] << 8), nf = buf[8] | (buf[9] << 8);
                        long need = 12 + (long)nf * (2 + (long)w * h * 2);
                        if (w > 0 && h > 0 && w <= LCD_W && h <= LCD_H && nf > 0 && need <= n) {
                            media = buf; media_w = w; media_h = h; media_nf = nf;
                            media_fi = 0; media_next = xTaskGetTickCount();
                        } else { free(buf); display_message("吧唧格式不符"); }
                    } else { if (buf) free(buf); display_message("没找到 badge.m8g"); }
                }
            }
            if (media_nf > 0) {                            // 逐帧播放（到点才换帧）
                TickType_t now = xTaskGetTickCount();
                if ((int32_t)(now - media_next) >= 0) {
                    const uint8_t *fp = media + 12 + (size_t)media_fi * (2 + (size_t)media_w * media_h * 2);
                    int delay = fp[0] | (fp[1] << 8);
                    display_blit_fit(fp + 2, media_w, media_h);   // 放大铺满屏
                    media_next = now + pdMS_TO_TICKS(delay < 20 ? 20 : delay);
                    media_fi = (media_fi + 1) % media_nf;
                }
            }
            if ((int32_t)(xTaskGetTickCount() - media_t0) > (int32_t)pdMS_TO_TICKS(MEDIA_TIMEOUT_MS)) {
                page = 0; page_dirty = true;               // 超时回脸
            }
            t++;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }
        // ── 设置页（LVGL）：脸/数值/媒体仍裸渲染，这页交给 LVGL，分时复用面板 ──
        if (page == 3) {
            if (page_dirty) {
                page_dirty = false;
                display_bubble(""); display_battery(-1, false);
                shown_bub[0] = 1; shown_bub[1] = 0; shown_bstate = -99;
                display_clear();
                lvgl_show_settings();                       // 切到设置屏，标脏全屏
            }
            lvgl_tick();                                   // 驱动 LVGL 渲染 + 输入
            if (g_badge_refresh) { g_badge_refresh = false; if (media) { free(media); media = NULL; media_nf = 0; } }
            if (g_lvgl_exit)     { g_lvgl_exit = false; page = 0; page_dirty = true; }
            t++;
            vTaskDelay(pdMS_TO_TICKS(15));
            continue;
        }

        // 离开媒体页/设置页：不释放，缓存留 PSRAM，下次进页秒开
        if (page_dirty) { display_clear(); page_dirty = false; last_m = NULL; }  // 回脸：整屏清残留并强制重画一帧

        // 事件覆盖：poll 触发的临时表情（载波/制式/上线），TTL 内盖住稳态
        const bool evt = (g_evt_until != 0) &&
                         ((int32_t)(xTaskGetTickCount() - g_evt_until) < 0);
        const bool offline = !g_net;     // WiFi 连不上网关 → 挂了

        const mood_t *m = petting ? (pet > 0.75f ? &PET_HI : &PET_LO)
                        : offline ? &MOODS[M_OFFLINE]
                        : evt     ? &MOODS[g_evt_mood]
                                  : &MOODS[g_mood];

        if (petting) {
            // 摸头：气泡带冒脉动爱心（越满足越多颗），松手后强制重画文字气泡
            display_hearts(pet > 0.95f ? 3 : (pet > 0.55f ? 2 : 1), (float)t * 0.18f);
            shown_bub[0] = 1; shown_bub[1] = 0;
        } else {
            // 想显示的气泡：连不上 / 事件 / 网络层动态气泡
            const char *want = offline ? "连不上…" : evt ? g_evt_bub : g_dyn_bub;
            if (strncmp(want, shown_bub, sizeof shown_bub) != 0) {   // 台词变了 → 刷气泡
                strncpy(shown_bub, want, sizeof shown_bub - 1);
                shown_bub[sizeof shown_bub - 1] = 0;
                display_bubble(want);
                ESP_LOGI(TAG, "mood=%s net=%d", m->name, (int)g_net);
            }
        }

        // 电量：摸头时亮出，松手清掉
        int bstate = petting ? (g_batt * 4 + (g_charging ? 1 : 0)) : -1;
        if (bstate != shown_bstate) {
            shown_bstate = bstate;
            if (petting) { power_read(); display_battery(g_batt, g_charging); }
            else display_battery(-1, false);
        }

        // 眨眼（仅启用 blink 的表情）
        float openK = 1.0f;
        if (m->blink) {
            if (blink == 0.0f) { if (--next_blink <= 0) blink = 0.001f; }
            if (blink > 0.0f) {
                blink += 0.30f;
                if (blink >= 2.0f) { blink = 0.0f; next_blink = 45 + (int)(frand() * 120); }
            }
            float ba = (blink > 0.0f) ? sinf(fminf(blink, 1.0f) * (float)M_PI) : 0.0f;
            openK = 1.0f - ba;
        } else { blink = 0.0f; }

        // 呼吸
        float by = sinf((float)t * 0.10f * m->breathe) * 4.0f;

        // 瞟眼
        if (--next_gaze <= 0) {
            gaze_target = (frand() * 2.0f - 1.0f) * 11.0f; gaze_hold = 22;
            next_gaze = 70 + (int)(frand() * 110);
        }
        if (gaze_hold > 0 && --gaze_hold == 0) gaze_target = 0.0f;
        gx += (gaze_target - gx) * 0.18f;

        // IMU：重力倾斜眼神 + 拿起唤醒 + 摇一摇
        float ax, ay, az;
        if (g_imu_ok && imu_read(&ax, &ay, &az)) {
            float target = ay * 34.0f;                    // 左右倾(ay) → 眼神横偏
            if (target > 17.0f) target = 17.0f; else if (target < -17.0f) target = -17.0f;
            tilt += (target - tilt) * 0.15f;

            float mot = fabsf(sqrtf(ax * ax + ay * ay + az * az) - 1.0f);   // 偏离 1g = 运动量
            bool moving = mot > 0.06f;
            if (moving) mood_note_motion();               // 有动静 → 不打盹/解除久闲
            TickType_t now = xTaskGetTickCount();
            if (mot > 0.55f && (int32_t)(now - shake_cd) > 0) {            // 摇一摇 → 哇！
                g_evt_mood = M_SURPRISED; strcpy(g_evt_bub, "哇！");
                g_evt_until = now + pdMS_TO_TICKS(2000);
                shake_cd = now + pdMS_TO_TICKS(2500);
            } else if (moving && !moving_prev && g_mood == M_SLEEPY &&
                       (int32_t)(now - shake_cd) > 0) {                    // 打盹时拿起 → 醒
                g_evt_mood = M_SURPRISED; strcpy(g_evt_bub, "唔…？");
                g_evt_until = now + pdMS_TO_TICKS(1500);
                shake_cd = now + pdMS_TO_TICKS(1800);
            }
            moving_prev = moving;
        }
        float tg = (fabsf(tilt) < 2.0f) ? 0.0f : tilt;   // 死区，去静止噪声

        // 朝向：被摸时朝手指蹭；否则正常瞟眼/抖 + 重力偏
        float gx_eff;
        if (petting) {
            float lean = (g_tx - LCD_W / 2) * 0.16f;
            if (lean > 16.0f) lean = 16.0f; else if (lean < -16.0f) lean = -16.0f;
            gx_eff = lean;
        } else {
            gx_eff = gx + tg;
            if (m->shake) gx_eff += sinf((float)t * 0.9f) * 3.0f;
        }

        // 亮度：sleepy 且非摸头 → 暗；否则常态
        int wantb = (!petting && m == &MOODS[M_SLEEPY]) ? BRIGHT_SLEEPY : g_user_bright;
        if (wantb != cur_bright) { display_brightness(wantb); cur_bright = wantb; }

        // 省电·按需渲染：画面实际没变（呼吸没跨像素/没眨眼/没瞟动/配件没落/没换表情）就不重画
        int by_q = (int)lroundf(by), gx_q = (int)lroundf(gx_eff), ok_q = (int)lroundf(openK * 20);
        int sw_q = m->sweat ? (int)((t * 2) % 92) : -1;
        int tr_q = m->tear  ? (int)((t * 2) % 56) : -1;
        bool need = (m != last_m) || (by_q != last_by) || (gx_q != last_gx) ||
                    (ok_q != last_ok) || (sw_q != last_sw) || (tr_q != last_tr);
        if (need) {
            display_face(m, t, by, gx_eff, openK);
            last_m = m; last_by = by_q; last_gx = gx_q; last_ok = ok_q; last_sw = sw_q; last_tr = tr_q;
        }
        t++;
        vTaskDelay(pdMS_TO_TICKS(need ? 12 : 70));   // 画了短歇；没画就多睡省电（计数节奏 ~12Hz 不变）
    }
}
