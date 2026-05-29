#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BTN_NONE = 0,
    BTN_A_SHORT,   // Approve
    BTN_B_SHORT,   // Deny
    BTN_A_LONG,    // Alt action
    BTN_B_LONG,    // Alt action
} button_event_t;

void button_init(void);
button_event_t button_poll(void);

#ifdef __cplusplus
}
#endif
