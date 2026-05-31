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

// Exact voicestick StickS3 ST7789 config
#define W CONFIG_DISPLAY_WIDTH
#define H CONFIG_DISPLAY_HEIGHT
#define LCD_X_GAP 52
#define LCD_Y_GAP 40
#define PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define BAR_H 20

static lv_display_t *g_display = NULL;
static lv_obj_t *g_screen = NULL;

// Status bar (xiaozhi-esp32 style — icon-based, overlapping layers)
static lv_obj_t *g_top_bar = NULL;
static lv_obj_t *g_status_bar = NULL;
static lv_obj_t *g_wifi_label = NULL;
static lv_obj_t *g_bat_label = NULL;
static lv_obj_t *g_status_label = NULL;

// Auth UI
static lv_obj_t *g_code_label = NULL;
static lv_obj_t *g_countdown_label = NULL;
static lv_obj_t *g_hint_label = NULL;

// Menu
static lv_obj_t *g_menu_panel = NULL;
static lv_obj_t *g_menu_title = NULL;
static lv_obj_t *g_menu_items[6] = {NULL};
static menu_page_t g_menu_page = MENU_NONE;
static int g_menu_idx = 0;
static bool g_initialized = false;
static auth_ui_state_t g_state = AUTH_STATE_IDLE;
static bool g_backlight_on = true;
static bool g_lang_en = false;

static int g_wifi_rssi = 0;
static int g_battery_pct = -1;
static bool g_battery_charging = false;
static bool g_wifi_connected = false;

#define COLOR(hex) lv_color_hex(hex)

// ── I18N ──────────────────────────────────────────
static const char *t(const char *zh, const char *en) { return g_lang_en ? en : zh; }

static void take_lock(void) { lvgl_port_lock(0); }
static void give_lock(void) { lvgl_port_unlock(); }

static void set_hidden(lv_obj_t *obj, bool hide) {
    if (!obj) return;
    if (hide) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

// ── Backlight ─────────────────────────────────────
void display_set_backlight(bool on) {
    g_backlight_on = on;
    if (on) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 128);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
}

void display_toggle_language(void) {
    g_lang_en = !g_lang_en;
}

// ── Menu helpers ──────────────────────────────────
static void menu_clear(void) {
    if (g_menu_panel) {
        lv_obj_delete(g_menu_panel);
        g_menu_panel = NULL;
        g_menu_title = NULL;
        for (int i = 0; i < 6; i++) g_menu_items[i] = NULL;
    }
}

static void menu_highlight(void) {
    for (int i = 0; i < 5; i++) {
        if (!g_menu_items[i]) continue;
        if (i == g_menu_idx)
            lv_obj_set_style_text_color(g_menu_items[i], COLOR(0xe94560), 0);
        else
            lv_obj_set_style_text_color(g_menu_items[i], COLOR(0xccccdd), 0);
    }
}

void display_show_menu(void) {
    if (!g_initialized) return;
    take_lock();
    menu_clear();
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);
    g_menu_page = MENU_MAIN;
    g_menu_idx = 0;

    g_menu_panel = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_menu_panel);
    lv_obj_set_size(g_menu_panel, W - 32, H - 40);
    lv_obj_set_style_bg_color(g_menu_panel, COLOR(0x13131f), 0);
    lv_obj_set_style_radius(g_menu_panel, 8, 0);
    lv_obj_set_style_pad_all(g_menu_panel, 16, 0);
    lv_obj_center(g_menu_panel);

    g_menu_title = lv_label_create(g_menu_panel);
    lv_obj_set_style_text_font(g_menu_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_menu_title, COLOR(0x888899), 0);
    lv_label_set_text(g_menu_title, t("\xe8\x8f\x9c\xe5\x8d\x95", "Menu"));

    static const char *items[] = {
        "\xe9\x87\x8d\xe6\x96\xb0\xe9\x85\x8d\xe7\xbd\xae",  // 重新配置
        "\xe4\xbd\xbf\xe7\x94\xa8\xe8\xaf\xb4\xe6\x98\x8e",  // 使用说明
        "\xe8\xaf\xad\xe8\xa8\x80 / Language",                // 语言
        "\xe5\x85\xb3\xe4\xba\x8e",                           // 关于
        "\xe8\xbf\x94\xe5\x9b\x9e",                           // 返回
    };
    static const char *items_en[] = {
        "Reconfigure", "How to Use", "Language", "About", "Back",
    };

    for (int i = 0; i < 5; i++) {
        g_menu_items[i] = lv_label_create(g_menu_panel);
        lv_obj_set_style_text_font(g_menu_items[i], &BUILTIN_TEXT_FONT, 0);
        lv_label_set_text(g_menu_items[i], g_lang_en ? items_en[i] : items[i]);
        if (i == 0)
            lv_obj_align_to(g_menu_items[i], g_menu_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 16);
        else
            lv_obj_align_to(g_menu_items[i], g_menu_items[i-1], LV_ALIGN_OUT_BOTTOM_LEFT, 0, 10);
    }
    menu_highlight();
    give_lock();
}

void display_menu_next(void) {
    if (g_menu_page != MENU_MAIN) return;
    g_menu_idx = (g_menu_idx + 1) % 5;
    take_lock();
    menu_highlight();
    give_lock();
}

void display_menu_select(void) {
    if (g_menu_page != MENU_MAIN) return;
    switch (g_menu_idx) {
    case 0: // Reconfig
        g_menu_page = MENU_RECONFIG;
        display_start_reconfig();
        menu_clear();
        break;
    case 1: // Usage
        g_menu_page = MENU_USAGE;
        display_show_usage();
        break;
    case 2: // Language
        g_menu_page = MENU_LANGUAGE;
        display_toggle_language();
        display_show_menu(); // refresh menu
        break;
    case 3: // About
        g_menu_page = MENU_ABOUT;
        display_show_about();
        break;
    case 4: // Back
        menu_clear();
        g_menu_page = MENU_NONE;
        display_show_idle();
        break;
    }
}

void display_menu_back(void) {
    if (g_menu_page == MENU_USAGE || g_menu_page == MENU_ABOUT) {
        display_show_menu();
    }
}

void display_show_usage(void) {
    if (!g_initialized) return;
    take_lock();
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);

    menu_clear();
    g_menu_panel = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_menu_panel);
    lv_obj_set_size(g_menu_panel, W - 32, H - 40);
    lv_obj_set_style_bg_color(g_menu_panel, COLOR(0x13131f), 0);
    lv_obj_set_style_radius(g_menu_panel, 8, 0);
    lv_obj_set_style_pad_all(g_menu_panel, 16, 0);
    lv_obj_center(g_menu_panel);

    g_menu_title = lv_label_create(g_menu_panel);
    lv_obj_set_style_text_font(g_menu_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_menu_title, COLOR(0xe94560), 0);
    lv_label_set_text(g_menu_title, t("\xe4\xbd\xbf\xe7\x94\xa8\xe8\xaf\xb4\xe6\x98\x8e", "How to Use"));

    // Body text
    const char *usage_zh =
        "AuthStick \xe6\x98\xaf\xe4\xb8\x80\xe6\xac\xbe\xe7\xa1\xac\xe4\xbb\xb6\xe8\xae\xa4\xe8\xaf\x81\xe7\xbb\x88\xe7\xab\xaf\xe3\x80\x82\n\n"
        "1. \xe9\x85\x8d\xe7\xbd\xaeWiFi\xe5\x92\x8c\xe8\xae\xa4\xe8\xaf\x81\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x9c\xb0\xe5\x9d\x80\n"
        "2. \xe8\xae\xbe\xe5\xa4\x87\xe6\x94\xb6\xe5\x88\xb0\xe9\xaa\x8c\xe8\xaf\x81\xe7\xa0\x81\xe5\x90\x8e\xe6\x98\xbe\xe7\xa4\xba\xe5\x9c\xa8\xe5\xb1\x8f\xe5\xb9\x95\xe4\xb8\x8a\n"
        "3. \xe5\x9c\xa8\xe7\x99\xbb\xe5\xbd\x95\xe9\xa1\xb5\xe8\xbe\x93\xe5\x85\xa5\xe9\xaa\x8c\xe8\xaf\x81\xe7\xa0\x81\xe6\x8f\x90\xe4\xba\xa4\n"
        "4. A\xe9\x94\xae\xe5\xbc\x80\xe5\x85\xb3\xe8\x9e\xa2\xe5\xb9\x95\n"
        "5. B\xe9\x94\xae\xe8\xbf\x9b\xe5\x85\xa5\xe8\x8f\x9c\xe5\x8d\x95";
    const char *usage_en =
        "AuthStick is a hardware authentication terminal.\n\n"
        "1. Configure WiFi and auth server address\n"
        "2. Device displays verification code on screen\n"
        "3. Enter the code on the login page to verify\n"
        "4. Press A to toggle screen\n"
        "5. Press B to enter menu";

    g_menu_items[0] = lv_label_create(g_menu_panel);
    lv_obj_set_style_text_font(g_menu_items[0], &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_menu_items[0], COLOR(0xccccdd), 0);
    lv_label_set_long_mode(g_menu_items[0], LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_menu_items[0], W - 64);
    lv_label_set_text(g_menu_items[0], g_lang_en ? usage_en : usage_zh);
    lv_obj_align_to(g_menu_items[0], g_menu_title, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 12);

    g_menu_page = MENU_USAGE;
    give_lock();
}

void display_show_about(void) {
    if (!g_initialized) return;
    take_lock();
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);

    menu_clear();
    g_menu_panel = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_menu_panel);
    lv_obj_set_size(g_menu_panel, W - 32, H - 40);
    lv_obj_set_style_bg_color(g_menu_panel, COLOR(0x13131f), 0);
    lv_obj_set_style_radius(g_menu_panel, 8, 0);
    lv_obj_set_style_pad_all(g_menu_panel, 16, 0);
    lv_obj_center(g_menu_panel);

    g_menu_title = lv_label_create(g_menu_panel);
    lv_obj_set_style_text_font(g_menu_title, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_menu_title, COLOR(0xe94560), 0);
    lv_label_set_text(g_menu_title, t("\xe5\x85\xb3\xe4\xba\x8e", "About"));

    const char *about_zh =
        "AuthStick\n"
        "v1.0\n"
        "by whitefirer";
    const char *about_en =
        "AuthStick\n"
        "v1.0\n"
        "by whitefirer";

    g_menu_items[0] = lv_label_create(g_menu_panel);
    lv_obj_set_style_text_font(g_menu_items[0], &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_menu_items[0], COLOR(0xccccdd), 0);
    lv_label_set_text(g_menu_items[0], g_lang_en ? about_en : about_zh);
    lv_obj_align_to(g_menu_items[0], g_menu_title, LV_ALIGN_OUT_BOTTOM_MID, 0, 20);
    lv_obj_set_style_text_align(g_menu_items[0], LV_TEXT_ALIGN_CENTER, 0);

    g_menu_page = MENU_ABOUT;
    give_lock();
}

bool display_is_menu_active(void) { return g_menu_page != MENU_NONE; }
menu_page_t display_get_menu(void) { return g_menu_page; }

void display_start_reconfig(void) {
    // Signal main.cpp to start reconfig — set a flag
    // The actual trigger is handled in main.cpp
}

// ── Init ──────────────────────────────────────────

void display_init(void) {
    // Backlight PWM
    ledc_timer_config_t bl_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_USE_RC_FAST_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&bl_timer));
    ledc_channel_config_t bl_ch = {
        .gpio_num = CONFIG_DISPLAY_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
        .flags = { .output_invert = false },
    };
    ESP_ERROR_CHECK(ledc_channel_config(&bl_ch));

    // SPI bus
    spi_bus_config_t bus_cfg = {
        .sclk_io_num = CONFIG_DISPLAY_PIN_SCLK,
        .mosi_io_num = CONFIG_DISPLAY_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = W * 24 * sizeof(lv_color16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Panel IO — 20MHz (voicestick proven speed)
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = CONFIG_DISPLAY_PIN_DC,
        .cs_gpio_num = CONFIG_DISPLAY_PIN_CS,
        .pclk_hz = PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &io));

    // ST7789 panel
    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = CONFIG_DISPLAY_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    // Panel init — exact voicestick sequence
    esp_lcd_panel_reset(panel);
    esp_lcd_panel_init(panel);
    esp_lcd_panel_invert_color(panel, true);
    esp_lcd_panel_mirror(panel, false, false);
    esp_lcd_panel_set_gap(panel, LCD_X_GAP, LCD_Y_GAP);
    // Clear screen to white (xiaozhi-esp32 pattern)
    uint16_t *white_buf = malloc(W * sizeof(uint16_t));
    if (white_buf) {
        for (int i = 0; i < W; i++) white_buf[i] = 0xFFFF;
        for (int y = 0; y < H; y++) {
            esp_lcd_panel_draw_bitmap(panel, 0, y, W, y + 1, white_buf);
        }
        free(white_buf);
    }

    esp_lcd_panel_disp_on_off(panel, true);

    // LVGL init
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    lvgl_port_init(&port_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = (uint32_t)(W * 24),
        .double_buffer = false,
        .trans_size = 0,
        .hres = (uint32_t)W,
        .vres = (uint32_t)H,
        .monochrome = false,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = 1,
            .buff_spiram = 0,
            .sw_rotate = 0,
            .swap_bytes = 1,
            .full_refresh = 0,
            .direct_mode = 0,
        },
    };

    g_display = lvgl_port_add_disp(&disp_cfg);
    if (!g_display) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return;
    }

    // Build UI
    take_lock();

    g_screen = lv_display_get_screen_active(g_display);
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_obj_set_style_text_font(g_screen, &BUILTIN_TEXT_FONT, 0);

    // ── Top bar (icon row, xiaozhi-esp32 style) ──
    g_top_bar = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_top_bar);
    lv_obj_set_size(g_top_bar, W, BAR_H);
    lv_obj_set_style_bg_color(g_top_bar, COLOR(0x0f0f1a), 0);
    lv_obj_set_style_bg_opa(g_top_bar, LV_OPA_50, 0);
    lv_obj_set_style_radius(g_top_bar, 0, 0);
    lv_obj_set_style_border_width(g_top_bar, 0, 0);
    lv_obj_set_style_pad_top(g_top_bar, 2, 0);
    lv_obj_set_style_pad_bottom(g_top_bar, 2, 0);
    lv_obj_set_style_pad_left(g_top_bar, 4, 0);
    lv_obj_set_style_pad_right(g_top_bar, 4, 0);
    lv_obj_set_flex_flow(g_top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(g_top_bar, LV_ALIGN_TOP_MID, 0, 0);

    g_wifi_label = lv_label_create(g_top_bar);
    lv_obj_set_style_text_font(g_wifi_label, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(g_wifi_label, COLOR(0x888899), 0);
    lv_label_set_text(g_wifi_label, "");

    g_bat_label = lv_label_create(g_top_bar);
    lv_obj_set_style_text_font(g_bat_label, &BUILTIN_ICON_FONT, 0);
    lv_obj_set_style_text_color(g_bat_label, COLOR(0x888899), 0);
    lv_label_set_text(g_bat_label, "");

    // ── Status bar (overlapping centered text, xiaozhi-esp32 style) ──
    g_status_bar = lv_obj_create(g_screen);
    lv_obj_remove_style_all(g_status_bar);
    lv_obj_set_size(g_status_bar, W, BAR_H);
    lv_obj_set_style_bg_opa(g_status_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(g_status_bar, 0, 0);
    lv_obj_set_style_border_width(g_status_bar, 0, 0);
    lv_obj_set_style_pad_all(g_status_bar, 0, 0);
    lv_obj_align(g_status_bar, LV_ALIGN_TOP_MID, 0, 0);

    g_status_label = lv_label_create(g_status_bar);
    lv_obj_set_style_text_font(g_status_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe0e0ee), 0);
    lv_obj_set_style_text_align(g_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_status_label, W - 8);
    lv_label_set_long_mode(g_status_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(g_status_label, "AuthStick");
    lv_obj_center(g_status_label);

    // ── Code label ───────────────────────────────────
    g_code_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_code_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_code_label, COLOR(0xffffff), 0);
    lv_obj_set_style_text_align(g_code_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_code_label, LV_SIZE_CONTENT);
    lv_label_set_text(g_code_label, "");
    lv_obj_align(g_code_label, LV_ALIGN_CENTER, 0, 0);
    set_hidden(g_code_label, true);

    // ── Countdown label ─────────────────────────────
    g_countdown_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_countdown_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_countdown_label, COLOR(0xe94560), 0);
    lv_obj_set_style_text_align(g_countdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_countdown_label, W - 16);
    lv_label_set_text(g_countdown_label, "");
    lv_obj_align_to(g_countdown_label, g_code_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);
    set_hidden(g_countdown_label, true);

    // ── Hint label ──────────────────────────────────
    g_hint_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_hint_label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_color(g_hint_label, COLOR(0x666688), 0);
    lv_obj_set_style_text_align(g_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_hint_label, W - 16);
    lv_label_set_text(g_hint_label, "");
    lv_obj_align(g_hint_label, LV_ALIGN_BOTTOM_MID, 0, -8);
    set_hidden(g_hint_label, true);

    give_lock();

    display_set_brightness(128);
    g_initialized = true;
    ESP_LOGI(TAG, "ST7789 init done %dx%d gap=%d,%d clk=%dMHz",
             W, H, LCD_X_GAP, LCD_Y_GAP, PIXEL_CLOCK_HZ / 1000000);
}

void display_set_brightness(uint8_t b) {
    if (!g_backlight_on) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, b);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

// ── Status bar ─────────────────────────────────────────

void display_set_wifi(int rssi) {
    g_wifi_rssi = rssi;
    display_draw_status_bar();
}

void display_set_wifi_connected(bool connected) {
    g_wifi_connected = connected;
    if (!connected) g_wifi_rssi = 0;
    display_draw_status_bar();
}

void display_set_config_mode(bool active) {
    if (active) {
        g_wifi_rssi = -1;
        g_wifi_connected = false;
    }
    display_draw_status_bar();
}

void display_set_battery(int pct, bool charging) {
    g_battery_pct = pct;
    g_battery_charging = charging;
    display_draw_status_bar();
}

void display_set_device_name(const char *name) {}

void display_draw_status_bar(void) {
    if (!g_initialized) return;
    take_lock();

    if (g_wifi_label) {
        const char *icon;
        lv_color_t color;
        if (g_wifi_rssi == -1) {
            icon = FONT_AWESOME_WIFI;
            color = COLOR(0x4ade80);
        } else if (!g_wifi_connected) {
            icon = FONT_AWESOME_WIFI_SLASH;
            color = COLOR(0x888899);
        } else if (g_wifi_rssi >= -65) {
            icon = FONT_AWESOME_WIFI;
            color = COLOR(0x4ade80);
        } else if (g_wifi_rssi >= -75) {
            icon = FONT_AWESOME_WIFI_FAIR;
            color = COLOR(0x4ade80);
        } else {
            icon = FONT_AWESOME_WIFI_WEAK;
            color = COLOR(0xe94560);
        }
        lv_label_set_text(g_wifi_label, icon);
        lv_obj_set_style_text_color(g_wifi_label, color, 0);
    }

    if (g_bat_label) {
        const char *icon;
        if (g_battery_pct < 0) {
            icon = FONT_AWESOME_BATTERY_EMPTY;
        } else if (g_battery_charging) {
            icon = FONT_AWESOME_BATTERY_BOLT;
        } else if (g_battery_pct <= 19) {
            icon = FONT_AWESOME_BATTERY_EMPTY;
        } else if (g_battery_pct <= 39) {
            icon = FONT_AWESOME_BATTERY_QUARTER;
        } else if (g_battery_pct <= 59) {
            icon = FONT_AWESOME_BATTERY_HALF;
        } else if (g_battery_pct <= 79) {
            icon = FONT_AWESOME_BATTERY_THREE_QUARTERS;
        } else {
            icon = FONT_AWESOME_BATTERY_FULL;
        }
        lv_label_set_text(g_bat_label, icon);
        lv_obj_set_style_text_color(g_bat_label,
            g_battery_charging ? COLOR(0x4ade80) : COLOR(0x888899), 0);
    }

    give_lock();
}

// ── UI states ───────────────────────────────────────────

void display_show_idle(void) {
    if (!g_initialized) return;
    g_state = AUTH_STATE_IDLE;
    menu_clear();
    g_menu_page = MENU_NONE;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, "AuthStick");
    lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);
    give_lock();
}

void display_show_connecting(void) {
    if (!g_initialized) return;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, t("\xe6\xad\xa3\xe5\x9c\xa8\xe8\xbf\x9e\xe6\x8e\xa5...", "Connecting..."));
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe94560), 0);
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);
    give_lock();
    display_set_brightness(128);
}

void display_show_wifi_config(const char *ap_ssid) {
    if (!g_initialized) return;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, "AuthStick");
    lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    lv_obj_set_style_text_font(g_code_label, &BUILTIN_TEXT_FONT, 0);
    char buf[64];
    snprintf(buf, sizeof(buf), "\xe8\xaf\xb7\xe8\xbf\x9e\xe6\x8e\xa5\xe7\x83\xad\xe7\x82\xb9 %s", ap_ssid);
    lv_label_set_text(g_code_label, buf);
    lv_obj_set_style_text_color(g_code_label, COLOR(0x4ade80), 0);
    set_hidden(g_code_label, false);
    if (g_countdown_label) {
        lv_label_set_text(g_countdown_label, "\xe5\xb9\xb6\xe8\xae\xbf\xe9\x97\xae" "192.168.4.1\xe9\x85\x8d\xe7\xbd\xaeWifi\xe5\x92\x8c\xe8\xae\xa4\xe8\xaf\x81\xe6\x9c\x8d\xe5\x8a\xa1\xe5\x9c\xb0\xe5\x9d\x80");
        lv_obj_set_style_text_color(g_countdown_label, COLOR(0x888899), 0);
        set_hidden(g_countdown_label, false);
    }
    set_hidden(g_hint_label, true);
    give_lock();
}

void display_show_code(const char *code, const char *service, int expires_in) {
    if (!g_initialized) return;
    g_state = AUTH_STATE_PENDING;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_obj_set_style_text_font(g_code_label, &font_digits_30_4, 0);

    // Translate known service names
    const char *svc = service;
    if (service && strcmp(service, "RegCode") == 0)
        svc = t("\xe6\xb3\xa8\xe5\x86\x8c\xe7\xa0\x81", "RegCode");
    if (svc && svc[0]) {
        lv_label_set_text(g_status_label, svc);
        lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    }

    lv_label_set_text(g_code_label, code);
    lv_obj_set_style_text_color(g_code_label, COLOR(0xffffff), 0);
    set_hidden(g_code_label, false);

    // Countdown: compute expiry HH:MM:SS
    if (g_countdown_label && expires_in > 0) {
        int64_t exp = esp_timer_get_time() + expires_in * 1000000LL;
        time_t et = exp / 1000000;
        struct tm tm;
        localtime_r(&et, &tm);
        char tbuf[48];
        snprintf(tbuf, sizeof(tbuf), t(
            "\xe6\x9c\x89\xe6\x95\x88\xe6\x9c\x9f\xe8\x87\xb3" " %02d:%02d:%02d",
            "Expires at %02d:%02d:%02d"), tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(g_countdown_label, tbuf);
        lv_obj_set_style_text_color(g_countdown_label, COLOR(0xe94560), 0);
        lv_obj_align_to(g_countdown_label, g_code_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
        set_hidden(g_countdown_label, false);
    }

    lv_label_set_text(g_hint_label, t(
        "A:\xe7\x86\x84\xe5\xb1\x8f  B:\xe8\x8f\x9c\xe5\x8d\x95",
        "A:Screen  B:Menu"));
    set_hidden(g_hint_label, false);
    give_lock();
}

void display_update_countdown(int remaining_s) {
    if (!g_initialized) return;
    take_lock();
    if (g_countdown_label) {
        time_t et = time(NULL) + remaining_s;
        struct tm tm;
        localtime_r(&et, &tm);
        char buf[48];
        snprintf(buf, sizeof(buf), t(
            "\xe6\x9c\x89\xe6\x95\x88\xe6\x9c\x9f\xe8\x87\xb3" " %02d:%02d:%02d",
            "Expires at %02d:%02d:%02d"), tm.tm_hour, tm.tm_min, tm.tm_sec);
        lv_label_set_text(g_countdown_label, buf);
    }
    give_lock();
}

void display_show_result(auth_ui_state_t result) {
    if (!g_initialized) return;
    g_state = result;
    take_lock();
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);

    if (result == AUTH_STATE_APPROVED) {
        lv_obj_set_style_bg_color(g_screen, COLOR(0x006000), 0);
        lv_label_set_text(g_status_label, t("\xe5\xb7\xb2\xe6\x89\xb9\xe5\x87\x86", "Approved"));
        lv_obj_set_style_text_color(g_status_label, COLOR(0xffffff), 0);
    } else {
        lv_obj_set_style_bg_color(g_screen, COLOR(0x800000), 0);
        lv_label_set_text(g_status_label, t("\xe5\xb7\xb2\xe6\x8b\x92\xe7\xbb\x9d", "Denied"));
        lv_obj_set_style_text_color(g_status_label, COLOR(0xffffff), 0);
    }
    give_lock();
}

void display_show_error(const char *msg) {
    if (!g_initialized) return;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x402000), 0);
    lv_label_set_text(g_status_label, t("\xe9\x94\x99\xe8\xaf\xaf", "Error"));
    lv_obj_set_style_text_color(g_status_label, COLOR(0xe94560), 0);
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);
    if (msg && msg[0]) {
        lv_label_set_text(g_hint_label, msg);
        lv_obj_set_style_text_color(g_hint_label, COLOR(0xeecc88), 0);
        lv_label_set_long_mode(g_hint_label, LV_LABEL_LONG_WRAP);
        lv_obj_set_height(g_hint_label, LV_SIZE_CONTENT);
        set_hidden(g_hint_label, false);
    }
    give_lock();
}

void display_show_wifi_status(const char *status, int rssi) {
    if (!g_initialized) return;
    take_lock();
    lv_obj_set_style_bg_color(g_screen, COLOR(0x1a1a2e), 0);
    lv_label_set_text(g_status_label, status ? status : t("\xe9\x85\x8d\xe7\xbd\x91\xe4\xb8\xad", "Configuring..."));
    lv_obj_set_style_text_color(g_status_label, COLOR(0x888899), 0);
    set_hidden(g_code_label, true);
    set_hidden(g_countdown_label, true);
    set_hidden(g_hint_label, true);
    if (rssi != 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Signal: %d dBm", rssi);
        lv_label_set_text(g_hint_label, buf);
        lv_obj_set_style_text_color(g_hint_label, COLOR(0x88aa88), 0);
        set_hidden(g_hint_label, false);
    }
    give_lock();
}
