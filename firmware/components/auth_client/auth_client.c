#include "auth_client.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "cJSON.h"

static const char *TAG = "auth_client";
static char g_server_url[128] = AUTH_SERVER_URL;
static char g_mac[DEVICE_MAC_LEN];

void auth_client_init(const char *server_url) {
    if (server_url && server_url[0]) {
        strncpy(g_server_url, server_url, sizeof(g_server_url) - 1);
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_mac, DEVICE_MAC_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "MAC=%s server=%s", g_mac, g_server_url);
}

void auth_client_get_mac(char *mac_out) {
    strncpy(mac_out, g_mac, DEVICE_MAC_LEN - 1);
}

static int http_get_json(const char *path, char *buf, int buf_len) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s?device=%s", g_server_url, path, g_mac);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_GET);
    esp_err_t err = esp_http_client_perform(client);
    int status = 0;
    if (err == ESP_OK) {
        status = esp_http_client_get_status_code(client);
        if (status == 200) {
            int len = esp_http_client_read(client, buf, buf_len - 1);
            if (len > 0) buf[len] = '\0';
        }
    }
    esp_http_client_cleanup(client);
    return status;
}

int auth_client_poll(auth_pending_code_t *codes, int max_count) {
    char buf[2048] = {0};
    int status = http_get_json("/api/stick/pending", buf, sizeof(buf));
    if (status != 200) return 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;
    cJSON *codes_arr = cJSON_GetObjectItem(root, "codes");
    if (!codes_arr) { cJSON_Delete(root); return 0; }

    int count = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, codes_arr) {
        if (count >= max_count) break;
        cJSON *c = cJSON_GetObjectItem(item, "code");
        cJSON *s = cJSON_GetObjectItem(item, "site_name");
        cJSON *e = cJSON_GetObjectItem(item, "expires_in");
        if (c) strncpy(codes[count].code, c->valuestring, 7);
        if (s) strncpy(codes[count].service_name, s->valuestring, 31);
        if (e) codes[count].expires_in = e->valueint;
        count++;
    }
    cJSON_Delete(root);
    return count;
}

int auth_client_approve(const char *code) {
    char url[256];
    snprintf(url, sizeof(url), "%s/api/stick/approve", g_server_url);
    char body[64];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"device\":\"%s\"}", code, g_mac);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return status == 200;
}

int auth_client_deny(const char *code) {
    char url[256];
    snprintf(url, sizeof(url), "%s/api/stick/deny", g_server_url);
    char body[64];
    snprintf(body, sizeof(body), "{\"code\":\"%s\",\"device\":\"%s\"}", code, g_mac);
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = (err == ESP_OK) ? esp_http_client_get_status_code(client) : 0;
    esp_http_client_cleanup(client);
    return status == 200;
}
