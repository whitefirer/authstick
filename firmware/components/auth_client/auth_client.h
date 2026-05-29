#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AUTH_SERVER_URL
#define AUTH_SERVER_URL CONFIG_AUTH_SERVER_URL
#endif
#define DEVICE_MAC_LEN 18

typedef struct {
    char code[8];
    char service_name[32];
    int expires_in;
} auth_pending_code_t;

// Init HTTP client with auth server URL
void auth_client_init(const char *server_url);

// Register device with server, get verification code. Returns HTTP status.
int auth_client_register(const char *server_url, const char *mac, char *code_out, int code_len);

// Poll for pending display codes. Returns count, fills codes[] up to max_count.
int auth_client_poll(auth_pending_code_t *codes, int max_count);

// Approve a code
int auth_client_approve(const char *code);

// Deny a code
int auth_client_deny(const char *code);

// Get device MAC for identification
void auth_client_get_mac(char *mac_out);

#ifdef __cplusplus
}
#endif
