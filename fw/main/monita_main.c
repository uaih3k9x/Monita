// 小圆脸固件 · Phase 3a：无口萌脸 六种眼神 + 待机动画
// 显示：Waveshare ESP32-S3-Touch-AMOLED-1.75（CO5300 466x466 QSPI）
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_ota_ops.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "cJSON.h"
#include "font_cjk.h"

// ── 触摸 CST9217（与 AXP 共用 I2C：SDA15/SCL14，地址 0x5A，RST40/INT11）──
#define TP_ADDR  0x5A
#define TP_RST   40
#define TP_INT   11

static const char *TAG = "monita";

// ── WiFi / 数据源 ──
#include "wifi_secret.h"          // 定义 WIFI_SSID / WIFI_PASS（本地文件，不提交）
#define FACE_URL  "http://192.168.2.254/face.json"

// ── OTA（终结 USB 烧录痛苦：之后只需 build → 拷 bin 到路由器 /www → 板子自更新）──
#define FW_VERSION   10                                      // 每次发版 +1
#define OTA_VER_URL  "http://192.168.2.254/monita.ver"       // 内容是个数字
#define OTA_BIN_URL  "http://192.168.2.254/monita-fw.bin"

// ── AXP2101 电源管理（I2C：SDA15 / SCL14，地址 0x34）──
#define AXP_ADDR     0x34
#define I2C_SDA      15
#define I2C_SCL      14

// ── 板级引脚（官方 BSP）──
#define LCD_HOST     SPI2_HOST
#define PIN_LCD_CS   12
#define PIN_LCD_PCLK 38
#define PIN_LCD_D0   4
#define PIN_LCD_D1   5
#define PIN_LCD_D2   6
#define PIN_LCD_D3   7
#define PIN_LCD_RST  39
#define LCD_W        466
#define LCD_H        466
#define LCD_X_GAP    0x06
#define LCD_Y_GAP    0x00

// ── 脸几何 ──
#define CXC   233
#define EYEY  227
#define GAP   92            // 两眼中心 = CXC ± GAP

// ── 脸矩形（覆盖最大眼形 surprised + 动态范围；只重画/推这块）──
#define RX0 45
#define RY0 120
#define RW  378
#define RH  224          // 脸矩形（含 surprised 眼下脸红），下方留给气泡
#define FACE_CHUNK 48    // 分块推屏的块高（内部 DMA 缓冲只需 LCD_W×FACE_CHUNK）

// 底部气泡带
#define BUB_Y0 350
#define BUB_H  62

// 顶部电量指示
#define TOPW 84
#define TOPH 26
#define TOPX ((LCD_W - TOPW) / 2)
#define TOPY 22

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

// 六态（移植自 index.html MOODS，眼形为主）
static const mood_t MOODS[] = {
    //          shape        hw  hh  glow brth  blink  shake  blush  sweat  tear   bubble
    {"happy",     EYE_ROUND,   38, 52, 26, 1.0f, true,  false, true,  false, false, ""},
    {"grin",      EYE_HAPPY,   46, 34, 24, 1.0f, false, false, true,  false, false, ""},
    {"busy",      EYE_STRAIN,  38, 28, 16, 1.9f, true,  false, false, true,  false, "clash 满载…"},
    {"surprised", EYE_WIDE,    48, 60, 34, 1.0f, true,  false, true,  false, false, "咦？新设备"},
    {"offline",   EYE_SQUEEZE, 40, 44, 14, 1.0f, false, true,  false, false, true,  "WAN 掉了…"},
    {"sleepy",    EYE_SLEEPY,  42, 30, 12, 0.5f, false, false, false, false, false, "z z z"},
    {"wilt",      EYE_STRAIN,  38, 26, 14, 0.7f, true,  false, false, false, false, ""},
};
#define N_MOODS (sizeof(MOODS) / sizeof(MOODS[0]))
enum { M_HAPPY, M_GRIN, M_BUSY, M_SURPRISED, M_OFFLINE, M_SLEEPY, M_WILT };  // 与 MOODS 顺序一致

static volatile int  g_mood = M_HAPPY;   // 目标表情（poll 任务设置）
static volatile bool g_net  = false;     // WiFi 是否拿到 IP

// 动态气泡（poll 任务填，渲染循环读）
static char g_dyn_bub[64] = "";          // 稳态气泡：busy 实时吞吐 / offline / sleepy 性格台词
static char g_evt_bub[64] = "";          // 事件气泡（载波/制式/上线）
static volatile int        g_evt_mood  = M_SURPRISED;  // 事件期间盖的表情
static volatile TickType_t g_evt_until = 0;            // 事件覆盖到期 tick（0=从未触发）

// blit DMA 完成信号量
static SemaphoreHandle_t s_blit_done;
static int64_t g_render_us, g_blit_us;
static uint16_t *g_face;    // 脸区域渲染目标（PSRAM, RW×RH）
static uint16_t *g_bub;     // 气泡带渲染目标（PSRAM, LCD_W×BUB_H）
static uint16_t *g_top;     // 顶部电量指示（PSRAM, TOPW×TOPH）
static uint16_t *g_chunk;   // 推屏用内部 DMA 小缓冲（LCD_W×FACE_CHUNK）—— 唯一占内部RAM的大块
static bool IRAM_ATTR on_color_done(esp_lcd_panel_io_handle_t io,
                                    esp_lcd_panel_io_event_data_t *e, void *ctx)
{
    BaseType_t hp = pdFALSE;
    xSemaphoreGiveFromISR(s_blit_done, &hp);
    return hp == pdTRUE;
}

static inline uint16_t sw16(uint16_t v) { return (uint16_t)((v << 8) | (v >> 8)); }

// 圆角盒 SDF（填充眼：round/wide/strain）
static inline float rbox_dist(float px, float py, float ex, float ey, float hw, float hh, float cr)
{
    float qx = fabsf(px - ex) - (hw - cr);
    float qy = fabsf(py - ey) - (hh - cr);
    float mx = qx > 0 ? qx : 0, my = qy > 0 ? qy : 0;
    float outside = (mx > 0.0f && my > 0.0f) ? sqrtf(mx * mx + my * my) : (mx + my);
    float inside = (qx > qy ? qx : qy); if (inside > 0) inside = 0;
    return outside + inside - cr;
}

// 点到线段距离（squeeze 折线用）
static inline float seg_dist(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float c2 = vx * vx + vy * vy;
    float t = c2 > 0 ? (vx * wx + vy * wy) / c2 : 0;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    float dx = px - (ax + t * vx), dy = py - (ay + t * vy);
    return sqrtf(dx * dx + dy * dy);
}

// 统一眼形 SDF：d<=0 为“眼内/笔画上”，d∈(0,glow) 为光晕
static float eye_sdf(eye_shape_t shape, float px, float py, float ex, float ey, float hw, float hh)
{
    switch (shape) {
        default:
        case EYE_ROUND:
        case EYE_WIDE:
        case EYE_STRAIN:
            return rbox_dist(px, py, ex, ey, hw, hh, fminf(hw, hh));
        case EYE_HAPPY: {            // ∩ 上拱描边
            const float th = 7.0f;
            float cxp = px < ex - hw ? ex - hw : (px > ex + hw ? ex + hw : px);
            float t = (cxp - ex) / hw;
            float yc = ey - hh * 0.7f * (1.0f - t * t);
            float dx = px - cxp, dy = py - yc;
            return sqrtf(dx * dx + dy * dy) - th;
        }
        case EYE_SLEEPY: {           // ∪ 下弯描边（闭眼）
            const float th = 7.0f;
            float cxp = px < ex - hw ? ex - hw : (px > ex + hw ? ex + hw : px);
            float t = (cxp - ex) / hw;
            float yc = ey + hh * 0.6f * (1.0f - t * t);
            float dx = px - cxp, dy = py - yc;
            return sqrtf(dx * dx + dy * dy) - th;
        }
        case EYE_SQUEEZE: {          // >< 两个折线
            const float th = 6.0f;
            float d1 = seg_dist(px, py, ex - hw, ey - 0.4f * hh, ex - 0.05f * hw, ey);
            float d2 = seg_dist(px, py, ex - 0.05f * hw, ey, ex - hw, ey + 0.4f * hh);
            float d3 = seg_dist(px, py, ex + hw, ey - 0.4f * hh, ex + 0.05f * hw, ey);
            float d4 = seg_dist(px, py, ex + 0.05f * hw, ey, ex + hw, ey + 0.4f * hh);
            float m = fminf(fminf(d1, d2), fminf(d3, d4));
            return m - th;
        }
    }
}

// 水滴：圆身 + 上尖
static inline bool in_drop(float px, float py, float cx, float cy, float r)
{
    const float dx = px - cx, dy = py - cy;
    if (dx * dx + dy * dy <= r * r) return true;
    if (dy < 0 && dy > -1.9f * r) {
        const float tt = -dy / (1.9f * r);
        if (fabsf(dx) <= r * 0.75f * (1.0f - tt)) return true;
    }
    return false;
}

// 在 strip 上盖一颗水滴（小包围盒，裁到脸矩形内，最上层）
static void stamp_drop(uint16_t *strip, float cx, float cy, float r, uint16_t color)
{
    int x0 = (int)(cx - r),         x1 = (int)(cx + r) + 1;
    int y0 = (int)(cy - 1.9f * r),  y1 = (int)(cy + r) + 1;
    if (x0 < RX0) x0 = RX0;
    if (x1 > RX0 + RW) x1 = RX0 + RW;
    if (y0 < RY0) y0 = RY0;
    if (y1 > RY0 + RH) y1 = RY0 + RH;
    const uint16_t sc = sw16(color);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (in_drop((float)x, (float)y, cx, cy, r))
                strip[(size_t)(y - RY0) * RW + (x - RX0)] = sc;
}

// 推一块到屏：NO_MEM 时重试（WiFi 满负荷会临时抢内部RAM，宁可丢帧不崩）
static void safe_blit(esp_lcd_panel_handle_t panel, int x0, int y0, int x1, int y1, const uint16_t *buf)
{
    for (int k = 0; k < 6; k++) {
        if (esp_lcd_panel_draw_bitmap(panel, x0, y0, x1, y1, buf) == ESP_OK) {
            xSemaphoreTake(s_blit_done, portMAX_DELAY);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(4));   // 等 WiFi 释放点内存再试
    }
    ESP_LOGW(TAG, "blit 跳过(内存紧张)");
}

// 把 PSRAM 源(宽 w 高 h) 经内部小缓冲 g_chunk 分块推到屏(x0,y0)
static void blit_psram(esp_lcd_panel_handle_t panel, const uint16_t *src, int x0, int y0, int w, int h)
{
    for (int y = 0; y < h; y += FACE_CHUNK) {
        int rows = (y + FACE_CHUNK <= h) ? FACE_CHUNK : (h - y);
        memcpy(g_chunk, src + (size_t)y * w, (size_t)rows * w * 2);
        safe_blit(panel, x0, y0 + y, x0 + w, y0 + y + rows, g_chunk);
    }
}

// 画一帧脸（渲进 PSRAM，再分块推屏）
static void draw_face_frame(esp_lcd_panel_handle_t panel, uint16_t *strip,
                            const mood_t *m, int t, float by, float gx, float openK)
{
    const float eyeYc = EYEY + by;
    const float hw = m->hw;
    const float glow = m->glow;
    const bool  filled = (m->shape == EYE_ROUND || m->shape == EYE_WIDE || m->shape == EYE_STRAIN);
    float hh = m->hh;
    if (filled) { hh *= openK; if (hh < 3.0f) hh = 3.0f; }   // 眨眼压扁（仅填充眼）
    const float exL = CXC - GAP + gx, exR = CXC + GAP + gx;
    const float cxg = CXC + gx;
    const float bbx = hw + glow + 6.0f;      // 横向包围盒
    const float bby = hh + glow + 6.0f;      // 纵向包围盒

    // 脸红：眼下两团柔和淡红
    const bool  blush   = m->blush;
    const float blush_y = eyeYc + hh + glow * 0.5f + 8.0f;
    const float BLUSH_R = 26.0f;
    const float bxL = exL - hw * 0.15f, bxR = exR + hw * 0.15f;

    const int64_t tr0 = esp_timer_get_time();
    for (int ry = 0; ry < RH; ry++) {
        uint16_t *out = strip + (size_t)ry * RW;
        const float py = (float)(RY0 + ry);
        const bool in_blush_row = blush && fabsf(py - blush_y) <= BLUSH_R;
        if (fabsf(py - eyeYc) > bby && !in_blush_row) { memset(out, 0x00, (size_t)RW * 2); continue; }
        for (int x = RX0; x < RX0 + RW; x++) {
            const float px = (float)x;
            const float ex = (px < cxg) ? exL : exR;
            if (fabsf(px - ex) > bbx) { out[x - RX0] = 0x0000; continue; }
            const float d = eye_sdf(m->shape, px, py, ex, eyeYc, hw, hh);

            // ① 底层：脸红（淡粉），作为基色
            int bR = 0, bG = 0, bB = 0;
            if (blush) {
                const float bcx = (px < cxg) ? bxL : bxR;
                const float ddx = px - bcx, ddy = py - blush_y;
                const float rr = ddx * ddx + ddy * ddy;
                if (rr < BLUSH_R * BLUSH_R) {
                    float bi = 1.0f - sqrtf(rr) / BLUSH_R; bi *= bi;
                    bR = (int)(14.0f * bi); bG = (int)(4.0f * bi); bB = (int)(5.0f * bi);
                }
            }
            // ② 眼/光晕叠加在脸红之上（光晕=加性混合，不是覆盖）
            uint16_t c;
            if (d <= 0.0f) {
                c = 0xFFFF;                                   // 眼/笔画盖一切
            } else if (d < glow) {
                float gi = 1.0f - d / glow; gi *= gi;
                int R = bR + (int)(31.0f * gi * 0.85f); if (R > 31) R = 31;
                int G = bG + (int)(63.0f * gi * 0.92f); if (G > 63) G = 63;
                int B = bB + (int)(31.0f * gi);          if (B > 31) B = 31;
                c = (uint16_t)((R << 11) | (G << 5) | B);
            } else {
                c = (uint16_t)((bR << 11) | (bG << 5) | bB);  // 仅脸红底色
            }
            out[x - RX0] = sw16(c);
        }
    }

    // 配件：汗滴 / 蓝泪（盖在最上层，按帧号下落）
    if (m->sweat) {
        float sx = exR + hw + 9.0f;
        float sy = (eyeYc - hh - 4.0f) + (float)((t * 2) % 92);
        stamp_drop(strip, sx, sy, 6.0f, (uint16_t)((20 << 11) | (44 << 5) | 31)); // 浅蓝白汗
    }
    if (m->tear) {
        float ty = eyeYc + hh * 0.5f + (float)((t * 2) % 56);
        uint16_t blue = (uint16_t)((9 << 11) | (28 << 5) | 31);
        stamp_drop(strip, exL, ty, 5.5f, blue);
        stamp_drop(strip, exR, ty, 5.5f, blue);
    }

    const int64_t tr1 = esp_timer_get_time();
    blit_psram(panel, strip, RX0, RY0, RW, RH);   // 分块推（内部缓冲小，WiFi 下不爆）
    g_render_us = tr1 - tr0;
    g_blit_us = esp_timer_get_time() - tr1;
}

static void clear_screen(esp_lcd_panel_handle_t panel)
{
    memset(g_chunk, 0x00, (size_t)LCD_W * FACE_CHUNK * 2);
    for (int y = 0; y < LCD_H; y += FACE_CHUNK) {
        const int rows = (y + FACE_CHUNK <= LCD_H) ? FACE_CHUNK : (LCD_H - y);
        safe_blit(panel, 0, y, LCD_W, y + rows, g_chunk);
    }
}

// ── 文字 / 气泡 ──────────────────────────────────────────
static uint32_t utf8_next(const char **pp)
{
    const uint8_t *p = (const uint8_t *)*pp;
    uint32_t cp; int n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
    else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
    else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
    else { cp = p[0]; n = 1; }
    for (int i = 1; i < n; i++) if ((p[i] & 0xC0) == 0x80) cp = (cp << 6) | (p[i] & 0x3F);
    *pp += n;
    return cp;
}

static int glyph_index(uint32_t cp)
{
    int lo = 0, hi = FONT_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        uint32_t v = FONT_CP[mid];
        if (v == cp) return mid;
        if (v < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

static int text_width(const char *s)
{
    int w = 0; const char *p = s;
    while (*p) { uint32_t cp = utf8_next(&p); int i = glyph_index(cp); w += (i >= 0 ? FONT_ADV[i] : FONT_W / 2) + 1; }
    return w;
}

static void draw_text(uint16_t *buf, int stride, int x, int y, const char *s, uint16_t color)
{
    const uint16_t sc = sw16(color);
    const char *p = s;
    while (*p) {
        uint32_t cp = utf8_next(&p);
        int i = glyph_index(cp);
        int adv = FONT_W / 2;
        if (i >= 0) {
            adv = FONT_ADV[i];
            const uint8_t *g = &FONT_BMP[(size_t)i * FONT_BYTES];
            for (int gy = 0; gy < FONT_H; gy++)
                for (int gx = 0; gx < FONT_W; gx++)
                    if (g[gy * FONT_ROWBYTES + (gx >> 3)] & (0x80 >> (gx & 7))) {
                        int px = x + gx, py = y + gy;
                        if (px >= 0 && px < stride && py >= 0 && py < BUB_H)
                            buf[(size_t)py * stride + px] = sc;
                    }
        }
        x += adv + 1;
    }
}

// 画底部气泡带：白圆角框 + 上尖 + 黑字；空串=清黑。每次切表情才调一次。
static void draw_bubble(esp_lcd_panel_handle_t panel, uint16_t *buf, const char *s)
{
    memset(buf, 0x00, (size_t)LCD_W * BUB_H * 2);
    if (s && s[0]) {
        const uint16_t W = sw16(0xFFFF);
        const int pad = 12, r = 8;
        int bw = text_width(s) + pad * 2, bh = FONT_H + 12;
        if (bw > LCD_W - 8) bw = LCD_W - 8;
        const int bx = (LCD_W - bw) / 2, by = (BUB_H - bh) / 2 + 5;
        // 上尖三角（指向脸）
        for (int ty = 0; ty < 8; ty++)
            for (int tx = -ty; tx <= ty; tx++) {
                int px = LCD_W / 2 + tx, py = by - 8 + ty;
                if (px >= 0 && px < LCD_W && py >= 0 && py < BUB_H) buf[(size_t)py * LCD_W + px] = W;
            }
        // 白圆角框
        for (int yy = 0; yy < bh; yy++)
            for (int xx = 0; xx < bw; xx++) {
                int dx = (xx < r) ? (r - xx) : (xx >= bw - r ? xx - (bw - 1 - r) : 0);
                int dy = (yy < r) ? (r - yy) : (yy >= bh - r ? yy - (bh - 1 - r) : 0);
                if (dx > 0 && dy > 0 && dx * dx + dy * dy > r * r) continue;
                int px = bx + xx, py = by + yy;
                if (px >= 0 && px < LCD_W && py >= 0 && py < BUB_H) buf[(size_t)py * LCD_W + px] = W;
            }
        draw_text(buf, LCD_W, bx + pad, by + 6, s, 0x0000);   // 黑字
    }
    blit_psram(panel, buf, 0, BUB_Y0, LCD_W, BUB_H);
}

// 顶部电量指示：电池外框 + 按 pct 填色（绿/黄/红，充电=青）。Phase C 摸头时调用。
__attribute__((unused))
static void draw_battery(esp_lcd_panel_handle_t panel, int pct, bool charging)
{
    memset(g_top, 0x00, (size_t)TOPW * TOPH * 2);
    if (pct >= 0) {
        const int bw = 60, bh = 22, bx = 6, by = (TOPH - bh) / 2;
        const uint16_t border = sw16(0xC618);                 // 浅灰边框
        for (int x = 0; x < bw; x++) { g_top[by * TOPW + bx + x] = border; g_top[(by + bh - 1) * TOPW + bx + x] = border; }
        for (int y = 0; y < bh; y++) { g_top[(by + y) * TOPW + bx] = border; g_top[(by + y) * TOPW + bx + bw - 1] = border; }
        for (int y = bh / 2 - 3; y < bh / 2 + 3; y++)          // 右侧正极帽
            for (int x = 0; x < 3; x++) g_top[(by + y) * TOPW + bx + bw + x] = border;
        uint16_t col = charging ? 0x07FF : (pct > 40 ? 0x07E0 : (pct > 15 ? 0xFFE0 : 0xF800));
        const uint16_t fc = sw16(col);
        int fw = (bw - 6) * pct / 100;
        for (int y = 2; y < bh - 2; y++)
            for (int x = 0; x < fw; x++) g_top[(by + y) * TOPW + bx + 2 + x] = fc;
    }
    blit_psram(panel, g_top, TOPX, TOPY, TOPW, TOPH);
}

static const co5300_lcd_init_cmd_t lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0}, {0x19, (uint8_t[]){0x10}, 1, 0}, {0x1C, (uint8_t[]){0xA0}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 1, 0}, {0xC4, (uint8_t[]){0x80}, 1, 0}, {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0}, {0x53, (uint8_t[]){0x20}, 1, 0}, {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600}, {0x29, NULL, 0, 0},
};

static esp_lcd_panel_handle_t display_init(void)
{
    s_blit_done = xSemaphoreCreateBinary();
    const spi_bus_config_t buscfg = CO5300_PANEL_BUS_QSPI_CONFIG(
        PIN_LCD_PCLK, PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3, LCD_W * LCD_H * 2);
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(PIN_LCD_CS, on_color_done, NULL);
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &io));

    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = { .use_qspi_interface = 1 },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    esp_lcd_panel_handle_t panel = NULL;
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    esp_lcd_panel_co5300_set_brightness(panel, 100);
    ESP_LOGI(TAG, "panel 就绪");
    return panel;
}

static inline float frand(void) { return (float)esp_random() / (float)UINT32_MAX; }

// ── AXP2101 电量（I2C）──
static volatile int  g_batt = -1;        // 电池百分比（-1=未知）
static volatile bool g_charging = false;
static i2c_master_dev_handle_t s_axp;
static i2c_master_bus_handle_t s_i2c_bus;   // AXP 与触摸共用

static esp_err_t axp_rd(uint8_t reg, uint8_t *val)
{
    return s_axp ? i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100) : ESP_FAIL;
}

static void axp_init(void)
{
    i2c_master_bus_config_t bc = {
        .i2c_port = -1, .sda_io_num = I2C_SDA, .scl_io_num = I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT, .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bc, &s_i2c_bus) != ESP_OK) { ESP_LOGW(TAG, "I2C 建失败"); return; }
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = AXP_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(s_i2c_bus, &dc, &s_axp) != ESP_OK) { ESP_LOGW(TAG, "AXP2101 挂载失败"); s_axp = NULL; return; }
    uint8_t id = 0, s0 = 0, s1 = 0, pct = 0xFF;
    axp_rd(0x03, &id); axp_rd(0x00, &s0); axp_rd(0x01, &s1); axp_rd(0xA4, &pct);
    ESP_LOGI(TAG, "AXP2101 id=0x%02x status0=0x%02x status1=0x%02x batt(0xA4)=%d", id, s0, s1, pct);
}

static void axp_read(void)
{
    uint8_t pct = 0xFF, st = 0;
    if (axp_rd(0xA4, &pct) == ESP_OK && pct <= 100) g_batt = pct;
    if (axp_rd(0x00, &st) == ESP_OK) g_charging = (st & 0x20) != 0;   // bit5 ≈ VBUS/充电（先这样，看 dump 再校）
}

// ── 触摸 CST9217（自写极简版，复用 s_i2c_bus）──
static i2c_master_dev_handle_t s_tp;
static volatile bool g_touched = false;
static volatile int  g_tx = 0, g_ty = 0;

static esp_err_t tp_rd(uint16_t reg, uint8_t *data, size_t len)
{
    if (!s_tp) return ESP_FAIL;
    uint8_t rb[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (i2c_master_transmit(s_tp, rb, 2, 100) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_receive(s_tp, data, len, 100);
}

// 写 16 位寄存器：先写地址，再写值（两段事务，与官方驱动一致）
static esp_err_t tp_wr(uint16_t reg, const uint8_t *val, size_t vlen)
{
    if (!s_tp) return ESP_FAIL;
    uint8_t a[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (i2c_master_transmit(s_tp, a, 2, 100) != ESP_OK) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(2));
    return i2c_master_transmit(s_tp, val, vlen, 100);
}

static void tp_init(void)
{
    if (!s_i2c_bus) return;
    gpio_config_t rc = { .pin_bit_mask = BIT64(TP_RST), .mode = GPIO_MODE_OUTPUT };
    gpio_config(&rc);
    gpio_config_t ic = { .pin_bit_mask = BIT64(TP_INT), .mode = GPIO_MODE_INPUT };
    gpio_config(&ic);
    gpio_set_level(TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(50));
    i2c_device_config_t dc = { .dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = TP_ADDR, .scl_speed_hz = 400000 };
    if (i2c_master_bus_add_device(s_i2c_bus, &dc, &s_tp) != ESP_OK) { s_tp = NULL; ESP_LOGW(TAG, "触摸挂载失败"); return; }

    // 初始化握手（照官方驱动 read_config）：进命令模式 + 读校验码/分辨率
    uint8_t cm[2] = { 0xD1, 0x01 };
    tp_wr(0xD101, cm, 2);  vTaskDelay(pdMS_TO_TICKS(10));
    uint8_t cc[4] = {0}, rs[4] = {0};
    tp_rd(0xD1FC, cc, 4);  // checkcode
    tp_rd(0xD1F8, rs, 4);  // resolution
    ESP_LOGI(TAG, "CST9217 checkcode=%02X%02X%02X%02X 分辨率=%dx%d",
             cc[0], cc[1], cc[2], cc[3], (rs[1] << 8) | rs[0], (rs[3] << 8) | rs[2]);

    uint8_t d[10] = {0};
    esp_err_t r = tp_rd(0xD000, d, sizeof(d));     // 自检：ack 应为 0xAB
    ESP_LOGI(TAG, "CST9217 自检 rd=%s ack=0x%02X points=%d", r == ESP_OK ? "OK" : "FAIL", d[6], d[5] & 0x7F);
}

static bool tp_read(int *x, int *y)
{
    uint8_t d[10] = {0};
    if (tp_rd(0xD000, d, sizeof(d)) != ESP_OK) return false;
    if (d[6] != 0xAB) return false;
    if ((d[5] & 0x7F) < 1) return false;
    if ((d[0] & 0x0F) != 0x06) return false;
    *x = (d[1] << 4) | (d[3] >> 4);
    *y = (d[2] << 4) | (d[3] & 0x0F);
    return true;
}

static void touch_task(void *arg)
{
    bool was = false; int cnt = 0;
    while (true) {
        int x = 0, y = 0;
        bool t = tp_read(&x, &y);
        if (t) { g_tx = x; g_ty = y; }
        if (t && !was) ESP_LOGI(TAG, "↓ 摸到 x=%d y=%d", x, y);
        else if (!t && was) ESP_LOGI(TAG, "↑ 松手");
        else if (t && (++cnt % 15 == 0)) ESP_LOGI(TAG, "… 摸 x=%d y=%d", x, y);
        g_touched = t; was = t;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ── OTA ──
static int http_get_int(const char *url, int def)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 4000 };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    int v = def;
    if (c && esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        char b[16] = {0}; int total = 0, n;
        while (total < (int)sizeof(b) - 1 &&
               (n = esp_http_client_read(c, b + total, sizeof(b) - 1 - total)) > 0) total += n;
        if (total > 0) { b[total] = 0; v = atoi(b); }
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    return v;
}

// 手动 OTA：HTTP 流式下载 → 写入备用分区（纯 HTTP，无 TLS 假设）
static esp_err_t do_ota(const char *url)
{
    esp_http_client_config_t cfg = { .url = url, .timeout_ms = 30000, .keep_alive_enable = true };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_FAIL;
    if (esp_http_client_open(c, 0) != ESP_OK) { esp_http_client_cleanup(c); return ESP_FAIL; }
    esp_http_client_fetch_headers(c);

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h = 0;
    esp_err_t err = part ? esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h) : ESP_FAIL;
    if (err != ESP_OK) { esp_http_client_close(c); esp_http_client_cleanup(c); return ESP_FAIL; }

    char *buf = malloc(4096);
    int n, written = 0;
    while ((n = esp_http_client_read(c, buf, 4096)) > 0) {
        if (esp_ota_write(h, buf, n) != ESP_OK) { err = ESP_FAIL; break; }
        written += n;
    }
    free(buf);
    if (n < 0) err = ESP_FAIL;
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (err == ESP_OK && esp_ota_end(h) == ESP_OK && esp_ota_set_boot_partition(part) == ESP_OK) {
        ESP_LOGW(TAG, "OTA 写入 %d 字节", written);
        return ESP_OK;
    }
    esp_ota_abort(h);
    return ESP_FAIL;
}

static void ota_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (!g_net) continue;
        axp_read();                                       // 顺便刷新电量
        int remote = http_get_int(OTA_VER_URL, -1);
        ESP_LOGI(TAG, "OTA 检查：远程 ver=%d，本机 v%d", remote, FW_VERSION);
        if (remote > FW_VERSION) {
            ESP_LOGW(TAG, "发现新固件 v%d（本机 v%d）→ OTA…", remote, FW_VERSION);
            if (do_ota(OTA_BIN_URL) == ESP_OK) { ESP_LOGW(TAG, "OTA 成功，重启"); esp_restart(); }
            else ESP_LOGE(TAG, "OTA 失败");
        }
    }
}

// ── WiFi ──
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) { g_net = false; esp_wifi_connect(); }
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { g_net = true; ESP_LOGI(TAG, "WiFi 已连，拿到 IP"); }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password) - 1);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ── 取数 + 情绪两轴映射 ──
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

static void poll_task(void *arg)
{
    while (true) {
        if (g_net) {
            esp_http_client_config_t cfg = { .url = FACE_URL, .timeout_ms = 4000 };
            esp_http_client_handle_t c = esp_http_client_init(&cfg);
            if (c && esp_http_client_open(c, 0) == ESP_OK) {
                esp_http_client_fetch_headers(c);
                char body[512]; int total = 0, n;
                while ((n = esp_http_client_read(c, body + total, sizeof(body) - 1 - total)) > 0) {
                    total += n;
                    if (total >= (int)sizeof(body) - 1) break;
                }
                body[total] = 0;
                if (total > 0) {
                    cJSON *j = cJSON_Parse(body);
                    if (j) {
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

                        cJSON_Delete(j);
                    }
                }
                esp_http_client_close(c);
            }
            if (c) esp_http_client_cleanup(c);
        }
        vTaskDelay(pdMS_TO_TICKS(4000));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "小圆脸 booting · Phase 5（WiFi 数据驱动）");
    esp_lcd_panel_handle_t panel = display_init();

    // 渲染目标放 PSRAM；只有 g_chunk（推屏小缓冲）占内部 DMA RAM → WiFi 满载也不爆
    g_face  = heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
    g_bub   = heap_caps_malloc((size_t)LCD_W * BUB_H * 2, MALLOC_CAP_SPIRAM);
    g_top   = heap_caps_malloc((size_t)TOPW * TOPH * 2, MALLOC_CAP_SPIRAM);
    g_chunk = heap_caps_malloc((size_t)LCD_W * FACE_CHUNK * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_face || !g_bub || !g_top || !g_chunk) { ESP_LOGE(TAG, "缓冲分配失败"); return; }
    clear_screen(panel);
    draw_bubble(panel, g_bub, MOODS[M_HAPPY].bubble);

    axp_init();                 // 电源管理芯片（后台读电量）
    axp_read();
    tp_init();                  // 触摸（摸摸头）

    // 联网 + 轮询 face.json + OTA 自更新
    esp_err_t nr = nvs_flash_init();
    if (nr == ESP_ERR_NVS_NO_FREE_PAGES || nr == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }
    wifi_start();
    xTaskCreate(poll_task,  "poll",  8192, NULL, 4, NULL);
    xTaskCreate(ota_task,   "ota",   8192, NULL, 3, NULL);
    xTaskCreate(touch_task, "touch", 4096, NULL, 5, NULL);   // 触摸读取（高优先级，响应快）
    ESP_LOGI(TAG, "固件版本 v%d", FW_VERSION);
    ESP_LOGI(TAG, "Phase 5：连 %s → 轮询 %s 驱动表情", WIFI_SSID, FACE_URL);

    int   t = 0;
    char  shown_bub[64] = {1, 0};               // 已显示的气泡文本（首帧强制画）
    int   shown_bstate = -99;
    float pet = 0.0f;                          // 摸摸头愉悦度
    int   next_blink = 50; float blink = 0.0f;
    int   next_gaze = 80, gaze_hold = 0; float gx = 0.0f, gaze_target = 0.0f;

    // 被摸态合成表情（∩ 弯眼 + 强腮红）
    static const mood_t PET_LO = {"pet", EYE_HAPPY, 46, 32, 26, 1.0f, false, false, true, false, false, "好舒服~"};
    static const mood_t PET_HI = {"pet", EYE_HAPPY, 48, 30, 28, 1.0f, false, false, true, false, false, "再摸摸~"};

    while (true) {
        // 摸摸头：愉悦度累积/衰减
        if (g_touched) pet += 0.06f; else pet -= 0.025f;
        if (pet < 0.0f) pet = 0.0f; else if (pet > 1.3f) pet = 1.3f;
        const bool petting = pet > 0.18f;

        // 事件覆盖：poll 触发的临时表情（载波/制式/上线），TTL 内盖住稳态
        const bool evt = (g_evt_until != 0) &&
                         ((int32_t)(xTaskGetTickCount() - g_evt_until) < 0);

        const mood_t *m = petting ? (pet > 0.75f ? &PET_HI : &PET_LO)
                        : evt     ? &MOODS[g_evt_mood]
                                  : &MOODS[g_mood];

        // 想显示的气泡：摸头用合成台词，事件用事件气泡，否则用网络层动态气泡
        const char *want = petting ? m->bubble : evt ? g_evt_bub : g_dyn_bub;

        if (strncmp(want, shown_bub, sizeof shown_bub) != 0) {   // 台词变了 → 刷气泡
            strncpy(shown_bub, want, sizeof shown_bub - 1);
            shown_bub[sizeof shown_bub - 1] = 0;
            draw_bubble(panel, g_bub, want);
            ESP_LOGI(TAG, "%s%s net=%d", petting ? "摸→" : "mood=", m->name, (int)g_net);
        }

        // 电量：摸头时亮出，松手清掉
        int bstate = petting ? (g_batt * 4 + (g_charging ? 1 : 0)) : -1;
        if (bstate != shown_bstate) {
            shown_bstate = bstate;
            if (petting) { axp_read(); draw_battery(panel, g_batt, g_charging); }
            else draw_battery(panel, -1, false);
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

        // 朝向：被摸时朝手指蹭；否则正常瞟眼/抖
        float gx_eff;
        if (petting) {
            float lean = (g_tx - CXC) * 0.16f;
            if (lean > 16.0f) lean = 16.0f; else if (lean < -16.0f) lean = -16.0f;
            gx_eff = lean;
        } else {
            gx_eff = gx;
            if (m->shake) gx_eff += sinf((float)t * 0.9f) * 3.0f;
        }

        draw_face_frame(panel, g_face, m, t, by, gx_eff, openK);

        if ((t % 30) == 0)
            ESP_LOGI(TAG, "frame %d [%s]: 总 %lldms (渲染%lld+推屏%lld)", t, m->name,
                     g_render_us / 1000 + g_blit_us / 1000, g_render_us / 1000, g_blit_us / 1000);
        t++;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
