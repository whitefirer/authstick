#include "button.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "button";

typedef struct {
    int gpio;
    bool last_state;
    int64_t press_since;  // 0 = not pressed
    button_event_t event;
} btn_ctx_t;

static btn_ctx_t g_btns[2];
static int64_t g_debounce_us = 50000; // 50ms

void button_init(void) {
    int pins[2] = {CONFIG_BTN_A_PIN, CONFIG_BTN_B_PIN};
    for (int i = 0; i < 2; i++) {
        g_btns[i].gpio = pins[i];
        g_btns[i].last_state = true;
        g_btns[i].press_since = 0;
        g_btns[i].event = BTN_NONE;
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
        gpio_set_pull_mode(pins[i], GPIO_PULLUP_ONLY);
    }
    ESP_LOGI(TAG, "Buttons init A=%d B=%d", pins[0], pins[1]);
}

button_event_t button_poll(void) {
    for (int i = 0; i < 2; i++) {
        bool cur = gpio_get_level(g_btns[i].gpio);
        if (cur != g_btns[i].last_state) {
            vTaskDelay(pdMS_TO_TICKS(1)); // coarse debounce
            cur = gpio_get_level(g_btns[i].gpio);
            if (cur != g_btns[i].last_state) {
                g_btns[i].last_state = cur;
                if (cur == 0) { // pressed (active low)
                    g_btns[i].press_since = esp_timer_get_time();
                } else { // released
                    int64_t held = esp_timer_get_time() - g_btns[i].press_since;
                    g_btns[i].press_since = 0;
                    button_event_t evt;
                    if (i == 0) {
                        evt = (held > CONFIG_BTN_LONG_PRESS_MS * 1000LL) ? BTN_A_LONG : BTN_A_SHORT;
                    } else {
                        evt = (held > CONFIG_BTN_LONG_PRESS_MS * 1000LL) ? BTN_B_LONG : BTN_B_SHORT;
                    }
                    ESP_LOGI(TAG, "Btn %d released after %lldms → %d", i, held / 1000, evt);
                    return evt;
                }
            }
        }
    }
    return BTN_NONE;
}
