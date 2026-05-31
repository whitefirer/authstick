#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AUTH_STATE_IDLE,
    AUTH_STATE_PENDING,
    AUTH_STATE_APPROVED,
    AUTH_STATE_DENIED,
} auth_ui_state_t;

typedef enum {
    OVERLAY_NONE = 0,
    OVERLAY_MENU,
    OVERLAY_USAGE,
    OVERLAY_ABOUT,
    OVERLAY_RESET_CONFIRM,
} overlay_page_t;

void display_init(void);
void display_set_brightness(uint8_t b);
void display_set_backlight(bool on);
void display_toggle_language(void);
void display_set_mac(const char *mac);

// Base pages (mutually exclusive)
void display_show_idle(void);
void display_show_connecting(void);
void display_show_wifi_config(const char *ap_ssid);
void display_show_code(const char *code, const char *service, int expires_in);
void display_show_result(auth_ui_state_t result);
void display_show_error(const char *msg);

// Overlay pages (stack on top of base)
void display_show_menu(void);
void display_menu_next(void);
void display_menu_select(void);
overlay_page_t display_get_overlay(void);
bool display_has_overlay(void);

// Status bar
void display_draw_status_bar(void);
void display_set_wifi(int rssi);
void display_set_wifi_connected(bool connected);
void display_set_config_mode(bool active);
void display_set_battery(int pct, bool charging);

#ifdef __cplusplus
}
#endif
