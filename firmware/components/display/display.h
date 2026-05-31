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
    MENU_NONE = 0,
    MENU_MAIN,
    MENU_RECONFIG,
    MENU_USAGE,
    MENU_LANGUAGE,
    MENU_ABOUT,
} menu_page_t;

void display_init(void);
void display_set_brightness(uint8_t b);
void display_set_backlight(bool on);

void display_show_idle(void);
void display_show_code(const char *code, const char *service, int expires_in);
void display_update_countdown(int remaining_s);
void display_show_result(auth_ui_state_t result);
void display_show_connecting(void);
void display_show_wifi_config(const char *ap_ssid);
void display_show_error(const char *msg);
void display_show_wifi_status(const char *status, int rssi);
void display_draw_status_bar(void);
void display_set_wifi(int rssi);
void display_set_wifi_connected(bool connected);
void display_set_config_mode(bool active);
void display_set_battery(int pct, bool charging);
void display_set_device_name(const char *name);

// Menu
menu_page_t display_get_menu(void);
void display_show_menu(void);
void display_menu_next(void);
void display_menu_select(void);
void display_menu_back(void);
void display_show_usage(void);
void display_show_about(void);
void display_toggle_language(void);
bool display_is_menu_active(void);
void display_start_reconfig(void);

#ifdef __cplusplus
}
#endif
