#include "lvgl_ui.h"
#include "lvgl.h"
#include "display.h"             // display_blit, LCD_W/H
#include "touch.h"               // g_touched / g_tx / g_ty
#include "net.h"                 // net_info
#include "mood.h"                // g_stat
#include "esp_timer.h"
#include "esp_heap_caps.h"

volatile int  g_user_bright   = 70;
volatile bool g_badge_refresh = false;
volatile bool g_lvgl_exit     = false;

static lv_obj_t *s_dash, *s_settings;          // 两个屏
static lv_obj_t *s_wifi;                        // 设置页 WiFi 标签
static lv_obj_t *d_ra, *d_rl, *d_sa, *d_sl, *d_info, *d_chart;  // 仪表盘控件
static lv_chart_series_t *d_dl, *d_ul;          // 下行(绿)/上行(琥珀)

static uint32_t tick_cb(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// flush：LVGL 画好的一块 → 字节序交换 → 复用面板分块推屏（同步，DMA 完才返回）
static void flush_cb(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    int w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
    lv_draw_sw_rgb565_swap(px, (uint32_t)w * h);
    display_blit(px, a->x1, a->y1, w, h);
    lv_display_flush_ready(d);
}

static void indev_cb(lv_indev_t *i, lv_indev_data_t *data)
{
    if (g_touched) { data->state = LV_INDEV_STATE_PRESSED; data->point.x = g_tx; data->point.y = g_ty; }
    else data->state = LV_INDEV_STATE_RELEASED;
}

static void slider_cb(lv_event_t *e)
{
    int v = lv_slider_get_value((lv_obj_t *)lv_event_get_target(e));
    g_user_bright = v; display_brightness(v);
}
static void refresh_cb(lv_event_t *e) { (void)e; g_badge_refresh = true; }
static void back_cb(lv_event_t *e)    { (void)e; g_lvgl_exit = true; }

// 只读弧形仪表：背景灰环 + 彩色指示，去掉旋钮、不可点
static lv_obj_t *make_gauge(lv_obj_t *parent, int x, int min, int max, lv_color_t col)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, 150, 150);
    lv_obj_align(a, LV_ALIGN_TOP_MID, x, 36);
    lv_arc_set_range(a, min, max);
    lv_arc_set_value(a, min);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, col, LV_PART_INDICATOR);
    return a;
}

static void build_dash(void)
{
    s_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_dash, lv_color_white(), 0);

    d_ra = make_gauge(s_dash, -90, -120, -60, lv_palette_main(LV_PALETTE_GREEN));
    d_rl = lv_label_create(s_dash); lv_obj_set_width(d_rl, 130);   // 固定宽，内容居中不挪
    lv_obj_set_style_text_align(d_rl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(d_rl, d_ra, LV_ALIGN_CENTER, 0, 0); lv_label_set_text(d_rl, "RSRP\n--");

    d_sa = make_gauge(s_dash, 90, -5, 30, lv_palette_main(LV_PALETTE_CYAN));
    d_sl = lv_label_create(s_dash); lv_obj_set_width(d_sl, 130);
    lv_obj_set_style_text_align(d_sl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align_to(d_sl, d_sa, LV_ALIGN_CENTER, 0, 0); lv_label_set_text(d_sl, "SINR\n--");

    d_chart = lv_chart_create(s_dash);
    lv_obj_set_size(d_chart, 330, 96);
    lv_obj_align(d_chart, LV_ALIGN_TOP_MID, 0, 210);
    lv_chart_set_type(d_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(d_chart, 50);
    lv_chart_set_range(d_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 200);   // KB/s（低量也看得见动）
    lv_obj_set_style_bg_color(d_chart, lv_color_hex(0x101010), 0);
    lv_obj_set_style_border_width(d_chart, 0, 0);
    lv_obj_set_style_line_width(d_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(d_chart, 0, 0, LV_PART_INDICATOR);        // 不画点
    d_dl = lv_chart_add_series(d_chart, lv_palette_main(LV_PALETTE_GREEN), LV_CHART_AXIS_PRIMARY_Y);
    d_ul = lv_chart_add_series(d_chart, lv_palette_main(LV_PALETTE_AMBER), LV_CHART_AXIS_PRIMARY_Y);

    d_info = lv_label_create(s_dash);
    lv_obj_set_width(d_info, 440);                 // 固定宽，内容居中不挪（防花屏/错位）
    lv_obj_set_style_text_align(d_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(d_info, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_label_set_text(d_info, "...");
}

// 取整到友好刻度（动态 Y 轴用），避免轴抖
static int nice_top(long v)
{
    static const int steps[] = {40, 60, 100, 150, 200, 300, 500, 800, 1200, 2000, 3500, 6000, 10000, 20000};
    for (unsigned i = 0; i < sizeof steps / sizeof steps[0]; i++)
        if (v <= steps[i]) return steps[i];
    return steps[sizeof steps / sizeof steps[0] - 1];
}

static void dash_update(lv_timer_t *t)
{
    (void)t;
    if (lv_screen_active() != s_dash) return;            // 只在仪表盘屏更新
    lv_arc_set_value(d_ra, g_stat.rsrp);
    lv_label_set_text_fmt(d_rl, "RSRP\n%d", g_stat.rsrp);
    lv_color_t rc = g_stat.rsrp >= -90 ? lv_palette_main(LV_PALETTE_GREEN)
                  : g_stat.rsrp >= -105 ? lv_palette_main(LV_PALETTE_YELLOW)
                                        : lv_palette_main(LV_PALETTE_RED);
    lv_obj_set_style_arc_color(d_ra, rc, LV_PART_INDICATOR);
    lv_arc_set_value(d_sa, g_stat.sinr);
    lv_label_set_text_fmt(d_sl, "SINR\n%d", g_stat.sinr);

    long dl = g_stat.dl / 1000, ul = g_stat.ul / 1000;   // KB/s（不夹，让轴自适应）
    lv_chart_set_next_value(d_chart, d_dl, (int32_t)dl);
    lv_chart_set_next_value(d_chart, d_ul, (int32_t)ul);

    // 动态 Y 轴：扫当前所有点的最大值 → 友好刻度
    int32_t *pdl = lv_chart_get_y_array(d_chart, d_dl);
    int32_t *pul = lv_chart_get_y_array(d_chart, d_ul);
    long mx = 0;
    for (int i = 0; i < 50; i++) {
        if (pdl[i] != LV_CHART_POINT_NONE && pdl[i] > mx) mx = pdl[i];
        if (pul[i] != LV_CHART_POINT_NONE && pul[i] > mx) mx = pul[i];
    }
    int top = nice_top(mx + mx / 4);
    lv_chart_set_range(d_chart, LV_CHART_AXIS_PRIMARY_Y, 0, top);

    lv_label_set_text_fmt(d_info, "DL %ld  UL %ld KB/s  max %d\n%s  B%s  %dC",
                          dl, ul, top, g_stat.mode[0] ? g_stat.mode : "-",
                          g_stat.band[0] ? g_stat.band : "-", g_stat.temp);
}

static void wifi_timer(lv_timer_t *t)
{
    (void)t;
    if (lv_screen_active() != s_settings) return;
    char ssid[40], ip[20]; int rssi;
    net_info(ssid, sizeof ssid, ip, sizeof ip, &rssi);
    lv_label_set_text_fmt(s_wifi, "WiFi: %s\nIP: %s\nSignal: %d dBm",
                          ssid[0] ? ssid : "-", ip[0] ? ip : "-", rssi);
}

static void build_settings(void)
{
    s_settings = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_settings, lv_color_black(), 0);
    lv_obj_set_style_text_color(s_settings, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(s_settings);
    lv_label_set_text(title, "Settings (LVGL)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *bl = lv_label_create(s_settings);
    lv_label_set_text(bl, "Brightness");
    lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_t *sl = lv_slider_create(s_settings);
    lv_obj_set_width(sl, 280);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, g_user_bright, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *btn = lv_button_create(s_settings);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -70, 175);
    lv_obj_t *btl = lv_label_create(btn); lv_label_set_text(btl, "Refresh badge"); lv_obj_center(btl);
    lv_obj_add_event_cb(btn, refresh_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(s_settings);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 90, 175);
    lv_obj_t *bk = lv_label_create(back); lv_label_set_text(bk, "Back"); lv_obj_center(bk);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    s_wifi = lv_label_create(s_settings);
    lv_obj_set_style_text_align(s_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_MID, 0, 235);
    lv_label_set_text(s_wifi, "WiFi: ...");
}

void lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    const size_t px = (size_t)LCD_W * 60;
    void *buf = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf, NULL, px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);

    build_dash();
    build_settings();
    lv_timer_create(dash_update, 1000, NULL);
    lv_timer_create(wifi_timer, 2000, NULL);
}

void lvgl_show_dash(void)     { lv_screen_load(s_dash);     lv_obj_invalidate(s_dash); }
void lvgl_show_settings(void) { lv_screen_load(s_settings); lv_obj_invalidate(s_settings); }
void lvgl_tick(void)          { lv_timer_handler(); }
