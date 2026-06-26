// 模拟太 · Monita 固件（日常昵称「小圆脸」）
// 名字取自《逆转裁判》希月心音的 モニ太——情绪随状态变、会替主人说心里话的胸挂小屏。
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
#include "font_cjk.h"
#include "i2c_bus.h"             // 共享 I2C 总线（电量 + 触摸）
#include "power.h"               // AXP2101 电量（g_batt / g_charging / power_read）
#include "touch.h"               // CST9217 触摸（g_touched / g_tx / g_ty / touch_task）
#include "net.h"                 // wifi_start / poll_task / g_net
#include "ota.h"                 // ota_task（OTA 自更新）
#include "mood.h"                // 情绪模型 + MOODS + mood_update
#include "version.h"             // FW_VERSION


static const char *TAG = "monita";



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

// 数值页：标题(含版本号) + 蜂窝指标，画进脸区域缓冲后整块推屏
static void draw_stats_page(esp_lcd_panel_handle_t panel, uint16_t *buf)
{
    memset(buf, 0x00, (size_t)RW * RH * 2);
    const uint16_t W = 0xFFFF;                 // 白（字节序对称）
    const uint16_t C = sw16(0x07FF);           // 青（标题）
    char s[48];
    int x = 16, y = 6, lh = FONT_H + 9;

    snprintf(s, sizeof s, "模拟太 v%d", FW_VERSION);
    draw_text(buf, RW, x, y, s, C); y += lh + 3;
    snprintf(s, sizeof s, "RSRP %d  SINR %d", g_stat.rsrp, g_stat.sinr);
    draw_text(buf, RW, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "载波 %dCC  B%s", g_stat.band_count, g_stat.band[0] ? g_stat.band : "?");
    draw_text(buf, RW, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "↓%ldk ↑%ldk", g_stat.dl / 1000, g_stat.ul / 1000);
    draw_text(buf, RW, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "%s %d℃ 电%d%%", g_stat.mode[0] ? g_stat.mode : "?", g_stat.temp, g_batt);
    draw_text(buf, RW, x, y, s, W);

    blit_psram(panel, buf, RX0, RY0, RW, RH);
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

    i2c_bus_init();             // 共享 I2C 总线（电量 + 触摸）
    power_init();               // 电源管理芯片（AXP2101，后台读电量）
    power_read();
    touch_init();               // 触摸（摸摸头）

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

    // 翻页 + 手势：快速轻点=翻页，长按≥300ms/滑动=摸头（区分开）
    int   page = 0;                             // 0=脸 1=数值页
    bool  page_dirty = false;
    bool  was_touch = false, moved = false;
    TickType_t down_t = 0; int down_x = 0, down_y = 0;
    int   stats_tick = 0;

    // 被摸态合成表情（∩ 弯眼 + 强腮红）
    static const mood_t PET_LO = {"pet", EYE_HAPPY, 46, 32, 26, 1.0f, false, false, true, false, false, "好舒服~"};
    static const mood_t PET_HI = {"pet", EYE_HAPPY, 48, 30, 28, 1.0f, false, false, true, false, false, "再摸摸~"};

    while (true) {
        // ── 触摸手势：轻点翻页 vs 长按摸头 ──
        bool tnow = g_touched;
        if (tnow && !was_touch) { down_t = xTaskGetTickCount(); down_x = g_tx; down_y = g_ty; moved = false; }
        if (tnow && (abs(g_tx - down_x) > 40 || abs(g_ty - down_y) > 40)) moved = true;
        if (!tnow && was_touch) {                          // 松手
            if (!moved && (xTaskGetTickCount() - down_t) < pdMS_TO_TICKS(300)) {
                page = (page + 1) % 2;                      // 快速轻点 → 翻页
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

        // ── 数值页：清气泡/电量，定期刷数值，跳过脸渲染 ──
        if (page == 1) {
            if (page_dirty) {
                draw_bubble(panel, g_bub, "");             // 清气泡带
                draw_battery(panel, -1, false);            // 清电量
                shown_bub[0] = 1; shown_bub[1] = 0;        // 回脸页时强制重画气泡
                shown_bstate = -99;
                stats_tick = 0; page_dirty = false;
            }
            if ((stats_tick++ % 50) == 0) draw_stats_page(panel, g_face);
            t++;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (page_dirty) page_dirty = false;                // 回到脸页：脸每帧都画，气泡靠 shown_bub 已重置

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
            if (petting) { power_read(); draw_battery(panel, g_batt, g_charging); }
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
