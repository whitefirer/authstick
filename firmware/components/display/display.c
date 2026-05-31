#include "display.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "font_awesome.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "lvgl.h"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_digits_30_4);

static const char *TAG = "display";

#define W CONFIG_DISPLAY_WIDTH
#define H CONFIG_DISPLAY_HEIGHT
#define LCD_X_GAP 52
#define LCD_Y_GAP 40
#define PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BAR_H 20

static lv_display_t *g_display = NULL;
static lv_obj_t *g_screen = NULL;
static bool g_initialized = false;
static auth_ui_state_t g_state = AUTH_STATE_IDLE;
static bool g_lang_en = false;
static bool g_backlight_on = true;
static char g_mac[18] = "";

// Status bar
static lv_obj_t *g_top_bar = NULL;
static lv_obj_t *g_wifi_label = NULL;
static lv_obj_t *g_bat_label = NULL;
static lv_obj_t *g_status_bar = NULL;
static lv_obj_t *g_status_label = NULL;
static int g_wifi_rssi = 0, g_battery_pct = -1;
static bool g_battery_charging = false, g_wifi_connected = false;

// Base page panels (one per page)
static lv_obj_t *g_page_base = NULL;
static lv_obj_t *g_label_code = NULL;
static lv_obj_t *g_label_sub = NULL;
static lv_obj_t *g_label_hint = NULL;

// Overlay panel
static lv_obj_t *g_overlay = NULL;
static lv_obj_t *g_overlay_title = NULL;
static lv_obj_t *g_overlay_items[6];
static overlay_page_t g_overlay_page = OVERLAY_NONE;
static int g_menu_idx = 0;

#define COLOR(hex) lv_color_hex(hex)
static const char *t(const char *zh, const char *en) { return g_lang_en ? en : zh; }

static void take_lock(void) { lvgl_port_lock(0); }
static void give_lock(void) { lvgl_port_unlock(); }
static void set_hidden(lv_obj_t *obj, bool hide) {
    if (obj) { if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN); }
}

// ── Base page helpers ─────────────────────────────────

static void close_base_page(void) {
    if (g_page_base) { lv_obj_delete(g_page_base); g_page_base = NULL; g_label_code = NULL; g_label_sub = NULL; g_label_hint = NULL; }
}

static void make_base_page(void) {
    close_base_page();
    g_page_base = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_page_base);
    lv_obj_set_size(g_page_base, W, H - BAR_H);
    lv_obj_set_style_bg_opa(g_page_base, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(g_page_base, 0, 0);
    lv_obj_set_style_pad_all(g_page_base, 0, 0);
    lv_obj_align(g_page_base, LV_ALIGN_BOTTOM_MID, 0, 0);
}

static void close_overlay(void) {
    if (g_overlay) { lv_obj_delete(g_overlay); g_overlay = NULL; g_overlay_title = NULL; for (int i = 0; i < 6; i++) g_overlay_items[i] = NULL; }
    g_overlay_page = OVERLAY_NONE;
}

static lv_obj_t *make_overlay_panel(void) {
    close_overlay();
    g_overlay = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_overlay);
    lv_obj_set_size(g_overlay, W - 8, H - 36);
    lv_obj_set_style_bg_color(g_overlay, COLOR(0x13131f), 0);
    lv_obj_set_style_radius(g_overlay, 8, 0);
    lv_obj_set_style_pad_all(g_overlay, 8, 0);
    lv_obj_align(g_overlay, LV_ALIGN_CENTER, 0, 0);
    return g_overlay;
}

// ── Backlight & settings ──────────────────────────────

void display_set_backlight(bool on) {
    g_backlight_on = on;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, on ? 128 : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

void display_toggle_language(void) { g_lang_en = !g_lang_en; }
void display_set_mac(const char *mac) { if (mac) { strncpy(g_mac, mac, 17); g_mac[17] = 0; } }
void display_set_brightness(uint8_t b) { if (g_backlight_on) { ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, b); ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0); } }
bool display_has_overlay(void) { return g_overlay_page != OVERLAY_NONE; }
overlay_page_t display_get_overlay(void) { return g_overlay_page; }

// ── Overlay: Menu ─────────────────────────────────────

static void overlay_highlight(void) {
    for (int i = 0; i < 5; i++) {
        if (!g_overlay_items[i]) continue;
        lv_obj_set_style_text_color(g_overlay_items[i], i == g_menu_idx ? COLOR(0xe94560) : COLOR(0xccccdd), 0);
    }
}

void display_show_menu(void) {
    if (!g_initialized) return;
    take_lock();
    close_overlay();
    set_hidden(g_page_base, true);
    make_overlay_panel();
    g_overlay_page = OVERLAY_MENU;
    g_menu_idx = 0;

    g_overlay_title = lv_label_create(g_overlay);
    lv_obj_set_style_text_font(g_overlay_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_overlay_title, COLOR(0x888899), 0);
    lv_label_set_text(g_overlay_title, t("\xe8\x8f\x9c\xe5\x8d\x95", "Menu"));

    static const char *items[] = {
        "\xe4\xbd\xbf\xe7\x94\xa8\xe8\xaf\xb4\xe6\x98\x8e",  // 使用说明
        "\xe8\xaf\xad\xe8\xa8\x80 / Language",
        "\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae",  // 恢复出厂
        "\xe5\x85\xb3\xe4\xba\x8e",
        "\xe8\xbf\x94\xe5\x9b\x9e",
    };
    static const char *items_en[] = {"How to Use", "Language", "Factory Reset", "About", "Back"};

    for (int i = 0; i < 5; i++) {
        g_overlay_items[i] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[i], &BUILTIN_TEXT_FONT, 0);
        lv_label_set_text(g_overlay_items[i], g_lang_en ? items_en[i] : items[i]);
        if (i == 0) lv_obj_align_to(g_overlay_items[i], g_overlay_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);
        else lv_obj_align_to(g_overlay_items[i], g_overlay_items[i-1], LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
    }
    g_overlay_items[5] = lv_label_create(g_overlay);
    lv_obj_set_style_text_font(g_overlay_items[5], &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_overlay_items[5], COLOR(0x666688), 0);
    lv_label_set_text(g_overlay_items[5], t("A:\xe7\xa1\xae\xe8\xae\xa4  B:\xe9\x80\x89\xe6\x8b\xa9", "A:OK  B:Select"));
    lv_obj_align(g_overlay_items[5], LV_ALIGN_BOTTOM_MID, 0, -4);
    overlay_highlight();
    give_lock();
}

void display_menu_next(void) {
    if (g_overlay_page != OVERLAY_MENU) return;
    g_menu_idx = (g_menu_idx + 1) % 5;
    take_lock(); overlay_highlight(); give_lock();
}

void display_menu_select(void) {
    if (g_overlay_page != OVERLAY_MENU) return;
    int idx = g_menu_idx;
    switch (idx) {
    case 0: // Usage
        take_lock(); close_overlay(); set_hidden(g_page_base, true); make_overlay_panel(); g_overlay_page = OVERLAY_USAGE;
        g_overlay_title = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_title, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_title, COLOR(0xe94560), 0);
        lv_label_set_text(g_overlay_title, t("\xe4\xbd\xbf\xe7\x94\xa8\xe8\xaf\xb4\xe6\x98\x8e", "How to Use"));
        g_overlay_items[0] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[0], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[0], COLOR(0xccccdd), 0);
        lv_obj_set_width(g_overlay_items[0], W - 24);
        lv_label_set_text(g_overlay_items[0], g_lang_en ?
            "1. Configure WiFi & server\n2. Device shows the code\n3. Enter code on login page\n4. A toggles screen\n5. B enters menu" :
            "1. \xe9\x85\x8dWiFi\xe5\x92\x8c\xe8\xae\xa4\xe8\xaf\x81\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x9c\xb0\xe5\x9d\x80\n2. \xe8\xae\xbe\xe5\xa4\x87\xe6\x98\xbe\xe7\xa4\xba\xe9\xaa\x8c\xe8\xaf\x81\xe7\xa0\x81\n3. \xe7\xbd\x91\xe9\xa1\xb5\xe8\xbe\x93\xe5\x85\xa5\xe6\x8f\x90\xe4\xba\xa4\n4. A\xe9\x94\xae\xe7\x86\x84\xe5\xb1\x8f\n5. B\xe9\x94\xae\xe8\x8f\x9c\xe5\x8d\x95");
        lv_obj_align_to(g_overlay_items[0], g_overlay_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 8);
        g_overlay_items[1] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[1], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[1], COLOR(0x666688), 0);
        lv_label_set_text(g_overlay_items[1], t("A/B:\xe8\xbf\x94\xe5\x9b\x9e\xe8\x8f\x9c\xe5\x8d\x95", "A/B:Back to menu"));
        lv_obj_align(g_overlay_items[1], LV_ALIGN_BOTTOM_MID, 0, -4);
        give_lock(); break;
    case 1: // Language
        display_toggle_language();
        take_lock(); close_overlay(); give_lock();
        display_show_menu();
        break;
    case 2: // Factory reset confirm
        take_lock(); close_overlay(); set_hidden(g_page_base, true); make_overlay_panel(); g_overlay_page = OVERLAY_RESET_CONFIRM;
        g_overlay_title = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_title, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_title, COLOR(0xe94560), 0);
        lv_label_set_text(g_overlay_title, t("\xe6\x81\xa2\xe5\xa4\x8d\xe5\x87\xba\xe5\x8e\x82\xe8\xae\xbe\xe7\xbd\xae", "Factory Reset"));
        g_overlay_items[0] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[0], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[0], COLOR(0xccccdd), 0);
        lv_label_set_text(g_overlay_items[0], t("\xe5\xb0\x86\xe6\xb8\x85\xe9\x99\xa4\xe6\x89\x80\xe6\x9c\x89\xe9\x85\x8d\xe7\xbd\xae\n\xe5\xb9\xb6\xe9\x87\x8d\xe5\x90\xaf\xe8\xae\xbe\xe5\xa4\x87", "Erase all settings\nand restart device"));
        lv_obj_align_to(g_overlay_items[0], g_overlay_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
        g_overlay_items[1] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[1], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[1], COLOR(0xe94560), 0);
        lv_label_set_text(g_overlay_items[1], t("A:\xe7\xa1\xae\xe8\xae\xa4  B:\xe5\x8f\x96\xe6\xb6\x88", "A:Confirm  B:Cancel"));
        lv_obj_align(g_overlay_items[1], LV_ALIGN_BOTTOM_MID, 0, -4);
        give_lock(); break;
    case 3: // About
        take_lock(); close_overlay(); set_hidden(g_page_base, true); make_overlay_panel(); g_overlay_page = OVERLAY_ABOUT;
        g_overlay_title = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_title, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_title, COLOR(0xe94560), 0);
        lv_label_set_text(g_overlay_title, t("\xe5\x85\xb3\xe4\xba\x8e", "About"));
        g_overlay_items[0] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[0], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[0], COLOR(0xccccdd), 0);
        lv_obj_set_style_text_align(g_overlay_items[0], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(g_overlay_items[0], W - 32);
        lv_label_set_long_mode(g_overlay_items[0], LV_LABEL_LONG_WRAP);
        { char buf[80]; snprintf(buf, sizeof(buf), "AuthStick v1.0\nby whitefirer\n\nMAC: %s", g_mac); lv_label_set_text(g_overlay_items[0], buf); }
        lv_obj_align(g_overlay_items[0], LV_ALIGN_CENTER, 0, -8);
        g_overlay_items[1] = lv_label_create(g_overlay);
        lv_obj_set_style_text_font(g_overlay_items[1], &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_overlay_items[1], COLOR(0x666688), 0);
        lv_label_set_text(g_overlay_items[1], t("A/B:\xe8\xbf\x94\xe5\x9b\x9e\xe8\x8f\x9c\xe5\x8d\x95", "A/B:Back to menu"));
        lv_obj_align(g_overlay_items[1], LV_ALIGN_BOTTOM_MID, 0, -4);
        give_lock(); break;
    case 4: // Back
        take_lock(); close_overlay(); set_hidden(g_page_base, false); give_lock(); break;
    }
}

// ── Base pages ────────────────────────────────────────

void display_show_idle(void) {
    if (!g_initialized) return;
    g_state = AUTH_STATE_IDLE;
    take_lock();
    close_base_page();
    make_base_page();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, "AuthStick");
    lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    g_label_hint = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_hint, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_hint, COLOR(0x666688), 0);
    lv_label_set_text(g_label_hint, t("A:\xe7\x86\x84\xe5\xb1\x8f  B:\xe8\x8f\x9c\xe5\x8d\x95", "A:Screen  B:Menu"));
    lv_obj_align(g_label_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    give_lock();
}

void display_show_connecting(void) {
    if (!g_initialized) return;
    take_lock();
    close_base_page();
    make_base_page();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, t("\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5...", "Connecting..."));
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe94560), 0);
    g_label_code = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_code, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_code, COLOR(0x4ade80), 0);
    lv_obj_set_style_text_align(g_label_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_label_code, t("\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5WiFi\n\xe8\xaf\xb7\xe7\xa8\x8d\xe5\x80\x99...", "Connecting to WiFi\nPlease wait..."));
    lv_obj_center(g_label_code);
    g_label_hint = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_hint, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_hint, COLOR(0x666688), 0);
    lv_label_set_text(g_label_hint, t("A:\xe7\x86\x84\xe5\xb1\x8f", "A:Screen Off"));
    lv_obj_align(g_label_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    give_lock();
    display_set_brightness(128);
}

void display_show_wifi_config(const char *ap_ssid) {
    if (!g_initialized) return;
    take_lock();
    close_base_page();
    make_base_page();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, "AuthStick");
    lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    g_label_code = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_code, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_code, COLOR(0x4ade80), 0);
    lv_obj_set_style_text_align(g_label_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_label_code, W - 32);
    lv_label_set_long_mode(g_label_code, LV_LABEL_LONG_WRAP);
    char buf[64]; snprintf(buf, sizeof(buf), "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\x83\xad\xe7\x82\xb9 %s", ap_ssid);
    lv_label_set_text(g_label_code, buf);
    lv_obj_center(g_label_code);
    g_label_sub = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_sub, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_sub, COLOR(0x888899), 0);
    lv_obj_set_style_text_align(g_label_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_label_sub, W - 32);
    lv_label_set_long_mode(g_label_sub, LV_LABEL_LONG_WRAP);
    lv_label_set_text(g_label_sub, "\xe5\xb9\xb6\xe8\xae\xbf\xe9\x97\xae" "192.168.4.1\xe9\x85\x8d\xe7\xbd\xaeWifi\xe5\x92\x8c\xe8\xae\xa4\xe8\xaf\x81\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x9c\xb0\xe5\x9d\x80");
    lv_obj_align_to(g_label_sub, g_label_code, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
    g_label_hint = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_hint, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_hint, COLOR(0x666688), 0);
    lv_label_set_text(g_label_hint, t("A:\xe7\x86\x84\xe5\xb1\x8f  B:\xe8\x8f\x9c\xe5\x8d\x95", "A:Screen  B:Menu"));
    lv_obj_align(g_label_hint, LV_ALIGN_BOTTOM_MID, 0, -4);
    give_lock();
}

void display_show_code(const char *code, const char *service, int expires_in) {
    if (!g_initialized || g_overlay_page != OVERLAY_NONE) return;
    g_state = AUTH_STATE_PENDING;
    take_lock();
    close_base_page();
    make_base_page();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);

    const char *svc = (service && strcmp(service, "RegCode") == 0) ? t("\xe6\xb3\xa8\xe5\x86\x8c\xe7\xa0\x81", "RegCode") : service;
    if (svc && svc[0]) {
        lv_label_set_text(g_status_label, svc);
        lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    }

    g_label_code = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_code, &font_digits_30_4, 0);
    lv_obj_set_style_text_color(g_label_code, COLOR(0xffffff), 0);
    lv_obj_set_style_text_align(g_label_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_label_code, LV_SIZE_CONTENT);
    lv_label_set_text(g_label_code, code);
    lv_obj_center(g_label_code);

    if (expires_in > 0) {
        time_t et = time(NULL) + expires_in;
        struct tm tm; localtime_r(&et, &tm);
        g_label_sub = lv_label_create(g_page_base);
        lv_obj_set_style_text_font(g_label_sub, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_label_sub, COLOR(0xe94560), 0);
        lv_obj_set_style_text_align(g_label_sub, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(g_label_sub, LV_SIZE_CONTENT);
        char buf[48];
        snprintf(buf, sizeof(buf), t("\xe6\x9c\x89\xe6\x95\x88\xe6\x9c\x9f\xe8\x87\xb3" " %04d/%02d/%02d\n%02d:%02d:%02d",
            "Expires %04d/%02d/%02d\n%02d:%02d:%02d"),
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(g_label_sub, buf);
        lv_obj_align_to(g_label_sub, g_label_code, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    }

    g_label_hint = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_hint, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_hint, COLOR(0x666688), 0);
    lv_obj_set_style_text_align(g_label_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(g_label_hint, t("A:\xe7\x86\x84\xe5\xb1\x8f  B:\xe8\x8f\x9c\xe5\x8d\x95", "A:Screen  B:Menu"));
    lv_obj_align(g_label_hint, LV_ALIGN_BOTTOM_MID, 0, -8);
    give_lock();
}

void display_show_result(auth_ui_state_t result) {
    if (!g_initialized) return;
    g_state = result;
    take_lock();
    close_base_page();
    make_base_page();
    if (result == AUTH_STATE_APPROVED) {
        lv_obj_set_style_bg_color(g_screen, COLOR(0x006000), 0);
        lv_label_set_text(g_status_label, t("\xe5\xb7\xb2\xe6\x89\xb9\xe5\x87\x86", "Approved"));
    } else {
        lv_obj_set_style_bg_color(g_screen, COLOR(0x800000), 0);
        lv_label_set_text(g_status_label, t("\xe5\xb7\xb2\xe6\x8b\x92\xe7\xbb\x9d", "Denied"));
    }
    lv_obj_set_style_text_color(g_status_label, COLOR(0xffffff), 0);
    g_label_code = lv_label_create(g_page_base);
    lv_obj_set_style_text_font(g_label_code, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_label_code, COLOR(0xffffff), 0);
    lv_label_set_text(g_label_code, "");
    lv_obj_center(g_label_code);
    give_lock();
}

void display_show_error(const char *msg) {
    if (!g_initialized) return;
    take_lock();
    close_base_page();
    make_base_page();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x402000), 0);
    lv_label_set_text(g_status_label, t("\xe9\x94\x99\xe8\xaf\xaf", "Error"));
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe94560), 0);
    if (msg && msg[0]) {
        g_label_code = lv_label_create(g_page_base);
        lv_obj_set_style_text_font(g_label_code, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_text_color(g_label_code, COLOR(0xeecc88), 0);
        lv_obj_set_style_text_align(g_label_code, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(g_label_code, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(g_label_code, W - 32);
        lv_label_set_text(g_label_code, msg);
        lv_obj_center(g_label_code);
    }
    give_lock();
}

// ── Status bar ─────────────────────────────────────────

void display_set_wifi(int rssi) { g_wifi_rssi = rssi; display_draw_status_bar(); }
void display_set_wifi_connected(bool c) { g_wifi_connected = c; if (!c) g_wifi_rssi = 0; display_draw_status_bar(); }
void display_set_config_mode(bool a) { if (a) { g_wifi_rssi = -1; g_wifi_connected = false; } display_draw_status_bar(); }
void display_set_battery(int pct, bool chg) { g_battery_pct = pct; g_battery_charging = chg; display_draw_status_bar(); }

void display_draw_status_bar(void) {
    if (!g_initialized) return;
    take_lock();
    if (g_wifi_label) {
        const char *icon; lv_color_t color;
        if (g_wifi_rssi == -1) { icon = FONT_AWESOME_WIFI; color = COLOR(0x4ade80); }
        else if (!g_wifi_connected) { icon = FONT_AWESOME_WIFI_SLASH; color = COLOR(0x888899); }
        else if (g_wifi_rssi >= -65) { icon = FONT_AWESOME_WIFI; color = COLOR(0x4ade80); }
        else if (g_wifi_rssi >= -75) { icon = FONT_AWESOME_WIFI_FAIR; color = COLOR(0x4ade80); }
        else { icon = FONT_AWESOME_WIFI_WEAK; color = COLOR(0xe94560); }
        lv_label_set_text(g_wifi_label, icon);
        lv_obj_set_style_text_color(g_wifi_label, color, 0);
    }
    if (g_bat_label) {
        const char *icon;
        if (g_battery_pct < 0) icon = FONT_AWESOME_BATTERY_EMPTY;
        else if (g_battery_charging) icon = FONT_AWESOME_BATTERY_BOLT;
        else if (g_battery_pct <= 19) icon = FONT_AWESOME_BATTERY_EMPTY;
        else if (g_battery_pct <= 39) icon = FONT_AWESOME_BATTERY_QUARTER;
        else if (g_battery_pct <= 59) icon = FONT_AWESOME_BATTERY_HALF;
        else if (g_battery_pct <= 79) icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
        else icon = FONT_AWESOME_BATTERY_FULL;
        lv_label_set_text(g_bat_label, icon);
        lv_obj_set_style_text_color(g_bat_label, g_battery_charging ? COLOR(0x4ade80) : COLOR(0x888899), 0);
    }
    give_lock();
}

// ── Init ──────────────────────────────────────────────

void display_init(void) {
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE, .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0, .freq_hz = 5000, .clk_cfg = LEDC_USE_RC_FAST_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = CONFIG_DISPLAY_PIN_BL, .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0, .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0, .duty = 0, .hpoint = 0, .flags = { .output_invert = false },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = CONFIG_DISPLAY_PIN_SCLK, .mosi_io_num = CONFIG_DISPLAY_PIN_MOSI,
        .miso_io_num = -1, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = W * 24 * sizeof(lv_color16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_DISPLAY_PIN_DC, .cs_gpio_num = CONFIG_DISPLAY_PIN_CS,
        .pclk_hz = PIXEL_CLOCK_HZ, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .spi_mode = 0, .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DISPLAY_PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB, .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));
    esp_lcd_panel_reset(panel); esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true); esp_lcd_panel_mirror(panel, false, false);
    esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP);
    uint16_t *white_buf = malloc(W * sizeof(uint16_t));
    if (white_buf) { for (int i = 0; i < W; i++) white_buf[i] = 0xFFFF; for (int y = 0; y < H; y++) esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + 1, white_buf); free(white_buf); }
    esp_lcd_panel_disp_on_off(panel, true);

    lv_init();
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG(); port_cfg.task_priority = 2;
    lvgl_port_init(&port_cfg);
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io, .panel_handle = panel, .control_handle = NULL,
        .buffer_size = (uint32_t)(W * 24), .double_buffer = false, .trans_size = 0,
        .hres = (uint32_t)W, .vres = (uint32_t)H, .monochrome = false,
        .rotation = { .swap_xy = false, .mirror_x = false, .mirror_y = false },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_dma = 1, .buff_spiram = 0, .sw_rotate = 0, .swap_bytes = 1, .full_refresh = 0, .direct_mode = 0 },
    };
    g_display = lvgl_port_add_disp(&disp_cfg);
    if (!g_display) { ESP_LOGE(TAG, "Failed to add LVGL display"); return; }

    take_lock();
    g_screen = lv_display_get_screen_active(g_display);
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_obj_set_style_text_font(g_screen, &BUILTIN_TEXT_FONT, 0);

    g_top_bar = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_top_bar);
    lv_obj_set_size(g_top_bar, W, BAR_H);
    lv_obj_set_style_bg_color(g_top_bar, COLOR(0x0f0f1a), 0);
    lv_obj_set_style_bg_opa(g_top_bar, LV_OPA_50, 0);
    lv_obj_set_style_radius(g_top_bar, 0, 0); lv_obj_set_style_border_width(g_top_bar, 0, 0);
    lv_obj_set_style_pad_top(g_top_bar, 2, 0); lv_obj_set_style_pad_bottom(g_top_bar, 2, 0);
    lv_obj_set_style_pad_left(g_top_bar, 4, 0); lv_obj_set_style_pad_right(g_top_bar, 4, 0);
    lv_obj_set_flex_flow(g_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(g_top_bar, LV_ALIGN_TOP_MID, 0, 0);
    g_wifi_label = lv_label_create(g_top_bar);
    lv_obj_set_style_text_font(g_wifi_label, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(g_wifi_label, COLOR(0x888899), 0); lv_label_set_text(g_wifi_label, "");
    g_bat_label = lv_label_create(g_top_bar);
    lv_obj_set_style_text_font(g_bat_label, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(g_bat_label, COLOR(0x888899), 0); lv_label_set_text(g_bat_label, "");

    g_status_bar = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_status_bar);
    lv_obj_set_size(g_status_bar, W, BAR_H);
    lv_obj_set_style_bg_opa(g_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(g_status_bar, 0, 0); lv_obj_set_style_border_width(g_status_bar, 0, 0);
    lv_obj_set_style_pad_all(g_status_bar, 0, 0);
    lv_obj_align(g_status_bar, LV_ALIGN_TOP_MID, 0, 0);
    g_status_label = lv_label_create(g_status_bar);
    lv_obj_set_style_text_font(g_status_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe0e0ee), 0);
    lv_obj_set_style_text_align(g_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_status_label, W - 8);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(g_status_label, "AuthStick"); lv_obj_center(g_status_label);
    give_lock();

    display_set_brightness(128);
    g_initialized = true;
    ESP_LOGI(TAG, "ST7789 init done %dx%d gap=%d,%d", W, H, LCD_X_GAP, LCD_Y_GAP);
}
