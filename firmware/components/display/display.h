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

void display_init(void);
void display_set_brightness(uint8_t b);
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

#ifdef __cplusplus
}
#endif
