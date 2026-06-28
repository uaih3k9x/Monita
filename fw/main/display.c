#include "display.h"
#include "mood.h"                // g_stat
#include "power.h"               // g_batt
#include "version.h"             // FW_VERSION
#include "font_cjk.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_co5300.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

static const char *TAG = "display";

// ── 板级引脚（官方 BSP）──
#define LCD_HOST     SPI2_HOST
#define PIN_LCD_CS   12
#define PIN_LCD_PCLK 38
#define PIN_LCD_D0   4
#define PIN_LCD_D1   5
#define PIN_LCD_D2   6
#define PIN_LCD_D3   7
#define PIN_LCD_RST  39
#define LCD_X_GAP    0x06
#define LCD_Y_GAP    0x00

// ── 脸几何 ──
#define CXC   (LCD_W / 2)        // 233
#define EYEY  227
#define GAP   92                 // 两眼中心 = CXC ± GAP

// ── 脸矩形（覆盖最大眼形 + 动态范围；只重画/推这块）──
#define RX0 45
#define RY0 120
#define RW  378
#define RH  224
#define FACE_CHUNK 48            // 分块推屏的块高（内部 DMA 缓冲只需 LCD_W×FACE_CHUNK）

// 底部气泡带
#define BUB_Y0 350
#define BUB_H  62

// 顶部电量指示
#define TOPW 84
#define TOPH 26
#define TOPX ((LCD_W - TOPW) / 2)
#define TOPY 22

static esp_lcd_panel_handle_t s_panel;
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
static void safe_blit(int x0, int y0, int x1, int y1, const uint16_t *buf)
{
    for (int k = 0; k < 6; k++) {
        if (esp_lcd_panel_draw_bitmap(s_panel, x0, y0, x1, y1, buf) == ESP_OK) {
            xSemaphoreTake(s_blit_done, portMAX_DELAY);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(4));   // 等 WiFi 释放点内存再试
    }
    ESP_LOGW(TAG, "blit 跳过(内存紧张)");
}

// 把 PSRAM 源(宽 w 高 h) 经内部小缓冲 g_chunk 分块推到屏(x0,y0)
static void blit_psram(const uint16_t *src, int x0, int y0, int w, int h)
{
    for (int y = 0; y < h; y += FACE_CHUNK) {
        int rows = (y + FACE_CHUNK <= h) ? FACE_CHUNK : (h - y);
        memcpy(g_chunk, src + (size_t)y * w, (size_t)rows * w * 2);
        safe_blit(x0, y0 + y, x0 + w, y0 + y + rows, g_chunk);
    }
}

// 画一帧脸（渲进 PSRAM，再分块推屏）
static void draw_face_frame(const mood_t *m, int t, float by, float gx, float openK)
{
    uint16_t *strip = g_face;
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
    blit_psram(strip, RX0, RY0, RW, RH);   // 分块推（内部缓冲小，WiFi 下不爆）
    g_render_us = tr1 - tr0;
    g_blit_us = esp_timer_get_time() - tr1;
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

// 把字串画进 buf（stride 宽 / h 高，超出裁掉）
static void draw_text(uint16_t *buf, int stride, int h, int x, int y, const char *s, uint16_t color)
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
                        if (px >= 0 && px < stride && py >= 0 && py < h)
                            buf[(size_t)py * stride + px] = sc;
                    }
        }
        x += adv + 1;
    }
}

// 画底部气泡带：白圆角框 + 上尖 + 黑字；空串=清黑。每次切表情才调一次。
static void draw_bubble(const char *s)
{
    uint16_t *buf = g_bub;
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
        draw_text(buf, LCD_W, BUB_H, bx + pad, by + 6, s, 0x0000);   // 黑字
    }
    blit_psram(buf, 0, BUB_Y0, LCD_W, BUB_H);
}

// 顶部电量指示：电池外框 + 按 pct 填色（绿/黄/红，充电=青）。pct<0=清除。
static void draw_battery(int pct, bool charging)
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
    blit_psram(g_top, TOPX, TOPY, TOPW, TOPH);
}

// 数值页底部：RSRP 趋势折线（手搓，RMIN..RMAX 映射到图高；-90 grin 阈值画虚线参考）
#define CHX 16
#define CHW 346
#define CHY 158
#define CHH 54
// 画一条折线（hist 环形缓冲映射 [rmin,rmax] → 图高），补纵向间隙不断线
static void chart_series(uint16_t *buf, const int16_t *hist, int rmin, int rmax, uint16_t col)
{
    int cnt = g_sig_cnt;
    if (cnt < 2) return;
    int prev_y = -1;
    for (int px = 0; px < CHW; px++) {
        float f = (float)px * (cnt - 1) / (CHW - 1);
        int i0 = (int)f; float fr = f - i0;
        int k0 = (g_sig_head - cnt + i0 + 2 * SIG_HIST_N) % SIG_HIST_N;
        int k1 = (k0 + 1) % SIG_HIST_N;
        float v = hist[k0] * (1 - fr) + hist[k1] * fr;
        if (v < rmin) v = rmin; else if (v > rmax) v = rmax;
        int y = CHY + CHH - (int)((v - rmin) * CHH / (rmax - rmin));
        if (y < CHY) y = CHY; else if (y > CHY + CHH) y = CHY + CHH;
        int y0 = (prev_y < 0) ? y : prev_y, y1 = y;
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        for (int yy = y0; yy <= y1; yy++) buf[(size_t)yy * RW + CHX + px] = col;
        prev_y = y;
    }
}

// 趋势图：RSRP(白) + SINR(青) 两条线；顶/底暗轴 + RSRP -90 阈值虚线
static void draw_sig_chart(uint16_t *buf)
{
    const uint16_t AX = sw16(0x4208), REF = sw16(0x3A49);
    for (int x = 0; x < CHW; x++) {
        buf[(size_t)CHY * RW + CHX + x] = AX;
        buf[(size_t)(CHY + CHH) * RW + CHX + x] = AX;
    }
    int yref = CHY + CHH - (-90 - (-120)) * CHH / ((-60) - (-120));   // RSRP -90 虚线
    if (yref > CHY && yref < CHY + CHH)
        for (int x = 0; x < CHW; x += 5) buf[(size_t)yref * RW + CHX + x] = REF;
    chart_series(buf, g_sinr_hist, -5, 30, sw16(0x07FF));   // SINR 青（下层）
    chart_series(buf, g_sig_hist, -120, -60, 0xFFFF);       // RSRP 白（上层，主）
}

// 数值页：标题(含版本号) + 蜂窝指标 + RSRP 趋势图，画进脸区域缓冲后整块推屏
static void draw_stats_page(void)
{
    uint16_t *buf = g_face;
    memset(buf, 0x00, (size_t)RW * RH * 2);
    const uint16_t W = 0xFFFF;                 // 白（字节序对称）
    const uint16_t C = sw16(0x07FF);           // 青（标题）
    char s[48];
    int x = 16, y = 4, lh = FONT_H + 5;

    snprintf(s, sizeof s, "模拟太 v%d", FW_VERSION);
    draw_text(buf, RW, RH, x, y, s, C); y += lh + 2;
    snprintf(s, sizeof s, "RSRP %d  SINR %d", g_stat.rsrp, g_stat.sinr);
    draw_text(buf, RW, RH, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "载波 %dCC B%s  设备%d", g_stat.band_count, g_stat.band[0] ? g_stat.band : "?", g_stat.clients);
    draw_text(buf, RW, RH, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "↓%ldk ↑%ldk", g_stat.dl / 1000, g_stat.ul / 1000);
    draw_text(buf, RW, RH, x, y, s, W); y += lh;
    snprintf(s, sizeof s, "%s %d℃ 电%d%%", g_stat.mode[0] ? g_stat.mode : "?", g_stat.temp, g_batt);
    draw_text(buf, RW, RH, x, y, s, W);

    draw_sig_chart(buf);          // 底部 RSRP 趋势

    blit_psram(buf, RX0, RY0, RW, RH);
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

static void panel_init(void)
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
    ESP_ERROR_CHECK(esp_lcd_new_panel_co5300(io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(s_panel, LCD_X_GAP, LCD_Y_GAP));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    esp_lcd_panel_co5300_set_brightness(s_panel, 100);
    ESP_LOGI(TAG, "panel 就绪");
}

// ── 公开接口 ──
void display_clear(void)
{
    memset(g_chunk, 0x00, (size_t)LCD_W * FACE_CHUNK * 2);
    for (int y = 0; y < LCD_H; y += FACE_CHUNK) {
        const int rows = (y + FACE_CHUNK <= LCD_H) ? FACE_CHUNK : (LCD_H - y);
        safe_blit(0, y, LCD_W, y + rows, g_chunk);
    }
}

bool display_init(void)
{
    panel_init();
    // 渲染目标放 PSRAM；只有 g_chunk（推屏小缓冲）占内部 DMA RAM → WiFi 满载也不爆
    g_face  = heap_caps_malloc((size_t)RW * RH * 2, MALLOC_CAP_SPIRAM);
    g_bub   = heap_caps_malloc((size_t)LCD_W * BUB_H * 2, MALLOC_CAP_SPIRAM);
    g_top   = heap_caps_malloc((size_t)TOPW * TOPH * 2, MALLOC_CAP_SPIRAM);
    g_chunk = heap_caps_malloc((size_t)LCD_W * FACE_CHUNK * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!g_face || !g_bub || !g_top || !g_chunk) { ESP_LOGE(TAG, "缓冲分配失败"); return false; }
    display_clear();
    return true;
}

void display_face(const mood_t *m, int t, float by, float gx, float openK)
{
    draw_face_frame(m, t, by, gx, openK);
    static int n = 0;
    if ((n++ % 30) == 0)
        ESP_LOGI(TAG, "frame [%s]: 总 %lldms (渲染%lld+推屏%lld)", m->name,
                 g_render_us / 1000 + g_blit_us / 1000, g_render_us / 1000, g_blit_us / 1000);
}

void display_bubble(const char *s)         { draw_bubble(s); }
void display_battery(int pct, bool charging) { draw_battery(pct, charging); }
void display_stats(void)                   { draw_stats_page(); }
void display_brightness(int pct)           { esp_lcd_panel_co5300_set_brightness(s_panel, pct); }

// 媒体页：raw 必须是 LCD_W×LCD_H 的 RGB565（已按面板字节序大端烤好）→ 直接整屏推
bool display_image(const uint8_t *raw, size_t len)
{
    if (len != (size_t)LCD_W * LCD_H * 2) { ESP_LOGW(TAG, "图尺寸不符 %u 字节", (unsigned)len); return false; }
    blit_psram((const uint16_t *)raw, 0, 0, LCD_W, LCD_H);
    return true;
}

// 把 w×h 的 RGB565 块推到屏 (x,y)（媒体页逐帧用）
void display_blit(const uint8_t *px, int x, int y, int w, int h)
{
    blit_psram((const uint16_t *)px, x, y, w, h);
}

// 把 w×h 帧最近邻放大到 ~MEDIA_FILL 居中铺满（媒体页：小源大显示）
#define MEDIA_FILL 440
void display_blit_fit(const uint8_t *px, int w, int h)
{
    const uint16_t *src = (const uint16_t *)px;
    int den = (w > h ? w : h);
    int ow = w * MEDIA_FILL / den, oh = h * MEDIA_FILL / den;
    if (ow > LCD_W) ow = LCD_W;
    if (oh > LCD_H) oh = LCD_H;
    int offx = (LCD_W - ow) / 2, offy = (LCD_H - oh) / 2;
    static int sxlut[LCD_W];
    for (int ox = 0; ox < ow; ox++) { int sx = ox * w / ow; sxlut[ox] = sx < w ? sx : w - 1; }
    for (int oy0 = 0; oy0 < oh; oy0 += FACE_CHUNK) {
        int rows = (oy0 + FACE_CHUNK <= oh) ? FACE_CHUNK : (oh - oy0);
        for (int r = 0; r < rows; r++) {
            int sy = (oy0 + r) * h / oh; if (sy >= h) sy = h - 1;
            const uint16_t *srow = src + (size_t)sy * w;
            uint16_t *drow = g_chunk + (size_t)r * ow;
            for (int ox = 0; ox < ow; ox++) drow[ox] = srow[sxlut[ox]];
        }
        safe_blit(offx, offy + oy0, offx + ow, offy + oy0 + rows, g_chunk);
    }
}

// 13×11 像素心点阵（星露谷风：粗描边 + 红填充 + 高光）。1=填充
#define HW 13
#define HH 11
static const uint8_t HEART[HH][HW] = {
    {0,0,1,1,1,0,0,0,1,1,1,0,0},
    {0,1,1,1,1,1,0,1,1,1,1,1,0},
    {1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1},
    {0,1,1,1,1,1,1,1,1,1,1,1,0},
    {0,0,1,1,1,1,1,1,1,1,1,0,0},
    {0,0,0,1,1,1,1,1,1,1,0,0,0},
    {0,0,0,0,1,1,1,1,1,0,0,0,0},
    {0,0,0,0,0,1,1,1,0,0,0,0,0},
    {0,0,0,0,0,0,1,0,0,0,0,0,0},
};

// 画一颗像素心：每格放大 cs×cs；空格但邻填充=描边，左上两格=高光；fade 0..1 整体变暗(淡出)
static void draw_pixheart(uint16_t *buf, int stride, int hgt, int ox, int oy, int cs, float fade)
{
    if (fade < 0.12f) return;
#define FC(r, g, b) sw16((((int)((r) * fade)) << 11) | (((int)((g) * fade)) << 5) | ((int)((b) * fade)))
    const uint16_t fill  = FC(28, 10, 8);    // 亮红
    const uint16_t outl  = FC(12, 4, 5);     // 深红描边
    const uint16_t shine = FC(31, 47, 25);   // 浅粉高光
#undef FC
    for (int gy = 0; gy < HH; gy++) {
        for (int gx = 0; gx < HW; gx++) {
            uint16_t col;
            if (HEART[gy][gx]) {
                col = ((gy == 1 && (gx == 3 || gx == 4)) || (gy == 2 && gx == 3)) ? shine : fill;
            } else {
                int nb = (gy > 0 && HEART[gy - 1][gx]) || (gy < HH - 1 && HEART[gy + 1][gx]) ||
                         (gx > 0 && HEART[gy][gx - 1]) || (gx < HW - 1 && HEART[gy][gx + 1]);
                if (!nb) continue;
                col = outl;
            }
            for (int py = 0; py < cs; py++)
                for (int px = 0; px < cs; px++) {
                    int X = ox + gx * cs + px, Y = oy + gy * cs + py;
                    if (X >= 0 && X < stride && Y >= 0 && Y < hgt) buf[(size_t)Y * stride + X] = col;
                }
        }
    }
}

// 摸头时气泡带冒 n 颗像素心，逐颗往上飘 + 淡出（星露谷摸小动物那种）
void display_hearts(int n, float phase)
{
    memset(g_bub, 0x00, (size_t)LCD_W * BUB_H * 2);
    if (n < 1) n = 1; else if (n > 3) n = 3;
    const int cs = 3, hw = HW * cs, hh = HH * cs, sp = hw + 30;
    const int x0 = LCD_W / 2 - ((n - 1) * sp + hw) / 2;
    const int travel = BUB_H - hh - 2;
    for (int i = 0; i < n; i++) {
        float ph = phase * 0.5f + i * 0.9f;
        float u = ph - floorf(ph);                            // 0..1 上升进度
        int oy = (BUB_H - hh) - (int)(u * travel);            // 从底升到顶
        float fade = 1.0f - u * 0.7f;                         // 越升越淡
        draw_pixheart(g_bub, LCD_W, BUB_H, x0 + i * sp, oy, cs, fade);
    }
    blit_psram(g_bub, 0, BUB_Y0, LCD_W, BUB_H);
}

// 设置页：标题(含版本) + 亮度滑块 + 刷新吧唧/重启 两按钮 + WiFi 信息（手搓）
// 控件局部坐标（与 display_settings_hit 一致）：滑块 y46..82；按钮 y90..126(刷新 x30..190 / 重启 x210..348)
// btn_hot: 0 无 / 2 刷新高亮 / 3 重启高亮
void display_settings(int bright, const char *ssid, const char *ip, int rssi, int btn_hot)
{
    uint16_t *buf = g_face;
    memset(buf, 0x00, (size_t)RW * RH * 2);
    const uint16_t W = 0xFFFF, C = sw16(0x07FF);
    char s[48];

    snprintf(s, sizeof s, "设置  v%d", FW_VERSION);
    draw_text(buf, RW, RH, (RW - text_width(s)) / 2, 2, s, C);
    snprintf(s, sizeof s, "亮度  %d%%", bright);
    draw_text(buf, RW, RH, 20, 30, s, W);

    // 滑块：底轨 + 填充 + 圆钮
    const int tx0 = 20, tw = RW - 40, ty = 56, th = 14;
    for (int y = ty; y < ty + th; y++)
        for (int x = tx0; x < tx0 + tw; x++) buf[(size_t)y * RW + x] = sw16(0x4208);
    int fw = tw * bright / 100;
    for (int y = ty; y < ty + th; y++)
        for (int x = tx0; x < tx0 + fw; x++) buf[(size_t)y * RW + x] = sw16(0x07FF);
    int kx = tx0 + fw, ky = ty + th / 2, kr = 11;
    for (int y = ky - kr; y <= ky + kr; y++)
        for (int x = kx - kr; x <= kx + kr; x++) {
            int dx = x - kx, dy = y - ky;
            if (dx * dx + dy * dy <= kr * kr && x >= 0 && x < RW && y >= 0 && y < RH) buf[(size_t)y * RW + x] = W;
        }

    // 两按钮
    const int by = 90, bh = 36;
    int b1x = 30, b1w = 160;     // 刷新吧唧
    uint16_t b1c = (btn_hot == 2) ? sw16(0x07E0) : sw16(0x39C7);
    for (int y = by; y < by + bh; y++) for (int x = b1x; x < b1x + b1w; x++) buf[(size_t)y * RW + x] = b1c;
    draw_text(buf, RW, RH, b1x + (b1w - text_width("刷新吧唧")) / 2, by + (bh - FONT_H) / 2, "刷新吧唧", (btn_hot == 2) ? sw16(0) : W);
    int b2x = 210, b2w = 138;    // 重启
    uint16_t b2c = (btn_hot == 3) ? sw16(0xF800) : sw16(0x39C7);
    for (int y = by; y < by + bh; y++) for (int x = b2x; x < b2x + b2w; x++) buf[(size_t)y * RW + x] = b2c;
    draw_text(buf, RW, RH, b2x + (b2w - text_width("重启")) / 2, by + (bh - FONT_H) / 2, "重启", W);

    // WiFi 信息
    snprintf(s, sizeof s, "WiFi %s", ssid[0] ? ssid : "-"); draw_text(buf, RW, RH, 20, 140, s, W);
    snprintf(s, sizeof s, "IP   %s", ip[0] ? ip : "-");     draw_text(buf, RW, RH, 20, 166, s, W);
    snprintf(s, sizeof s, "信号 %d dBm", rssi);             draw_text(buf, RW, RH, 20, 192, s, W);

    blit_psram(buf, RX0, RY0, RW, RH);
}

// 【调试】触摸校准：在固件以为的触点处画红十字 + 显示原始坐标（整屏）
void display_touchdot(int sx, int sy)
{
    uint16_t *buf = g_face;
    memset(buf, 0x00, (size_t)RW * RH * 2);
    char s[40]; snprintf(s, sizeof s, "T %d,%d", sx, sy);
    draw_text(buf, RW, RH, 8, 4, s, sw16(0x07FF));
    int lx = sx - RX0, ly = sy - RY0;          // 转脸缓冲局部坐标
    const uint16_t R = sw16(0xF800);
    for (int d = -12; d <= 12; d++) {
        if (lx + d >= 0 && lx + d < RW && ly >= 0 && ly < RH) buf[(size_t)ly * RW + (lx + d)] = R;
        if (ly + d >= 0 && ly + d < RH && lx >= 0 && lx < RW) buf[(size_t)(ly + d) * RW + lx] = R;
    }
    blit_psram(buf, RX0, RY0, RW, RH);
}

// 设置页命中检测（屏幕坐标）：0=空白 1=滑块(*bright) 2=刷新吧唧 3=重启
int display_settings_hit(int sx, int sy, int *bright)
{
    int lx = sx - RX0, ly = sy - RY0;
    if (ly >= 46 && ly <= 82 && lx >= 8 && lx <= 358) {
        int b = (lx - 20) * 100 / (RW - 40); if (b < 0) b = 0; else if (b > 100) b = 100;
        if (bright) *bright = b;
        return 1;
    }
    if (ly >= 90 && ly <= 126) {
        if (lx >= 30 && lx <= 190) return 2;
        if (lx >= 210 && lx <= 348) return 3;
    }
    return 0;
}

// 屏中央一行白字（画进脸区域缓冲，居中）
void display_message(const char *s)
{
    memset(g_face, 0x00, (size_t)RW * RH * 2);
    int w = text_width(s);
    draw_text(g_face, RW, RH, (RW - w) / 2, (RH - FONT_H) / 2, s, 0xFFFF);
    blit_psram(g_face, RX0, RY0, RW, RH);
}
