#include "lvgl_ui.h"
#include "lvgl.h"
#include "display.h"             // display_blit, LCD_W/H
#include "touch.h"               // g_touched / g_tx / g_ty
#include "net.h"                 // net_info
#include "esp_timer.h"
#include "esp_heap_caps.h"

volatile int  g_user_bright   = 70;
volatile bool g_badge_refresh = false;
volatile bool g_lvgl_exit     = false;

static lv_obj_t *s_wifi;

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

static void wifi_timer(lv_timer_t *t)
{
    (void)t;
    char ssid[40], ip[20]; int rssi;
    net_info(ssid, sizeof ssid, ip, sizeof ip, &rssi);
    lv_label_set_text_fmt(s_wifi, "WiFi: %s\nIP: %s\nSignal: %d dBm",
                          ssid[0] ? ssid : "-", ip[0] ? ip : "-", rssi);
}

void lvgl_init(void)
{
    lv_init();
    lv_tick_set_cb(tick_cb);

    lv_display_t *disp = lv_display_create(LCD_W, LCD_H);
    const size_t px = (size_t)LCD_W * 60;     // 部分缓冲 60 行
    void *buf = heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    lv_display_set_buffers(disp, buf, NULL, px * 2, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush_cb);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, indev_cb);

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings (LVGL)");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *bl = lv_label_create(scr);
    lv_label_set_text(bl, "Brightness");
    lv_obj_align(bl, LV_ALIGN_TOP_MID, 0, 95);
    lv_obj_t *sl = lv_slider_create(scr);
    lv_obj_set_width(sl, 280);
    lv_slider_set_range(sl, 5, 100);
    lv_slider_set_value(sl, g_user_bright, LV_ANIM_OFF);
    lv_obj_align(sl, LV_ALIGN_TOP_MID, 0, 130);
    lv_obj_add_event_cb(sl, slider_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *btn = lv_button_create(scr);
    lv_obj_align(btn, LV_ALIGN_TOP_MID, -70, 175);
    lv_obj_t *btl = lv_label_create(btn);
    lv_label_set_text(btl, "Refresh badge");
    lv_obj_center(btl);
    lv_obj_add_event_cb(btn, refresh_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back = lv_button_create(scr);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 90, 175);
    lv_obj_t *bk = lv_label_create(back);
    lv_label_set_text(bk, "Back");
    lv_obj_center(bk);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);

    s_wifi = lv_label_create(scr);
    lv_obj_set_style_text_align(s_wifi, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_wifi, LV_ALIGN_TOP_MID, 0, 235);
    lv_label_set_text(s_wifi, "WiFi: ...");
    lv_timer_create(wifi_timer, 2000, NULL);
    wifi_timer(NULL);
}

void lvgl_enter(void) { lv_obj_invalidate(lv_screen_active()); }
void lvgl_tick(void)  { lv_timer_handler(); }
