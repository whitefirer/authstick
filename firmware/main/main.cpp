#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "auth_client.h"
#include "display.h"
#include "button.h"
#include "driver/i2c_master.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "esp_network.h"

static const char *TAG = "main";

#define POLL_INTERVAL_MS    3000
#define RESULT_SHOW_MS      2000
#define REG_POLL_INTERVAL_MS 3000

// ── M5PM1 PMIC ──────────────────────────────────────────

#define M5PM1_ADDR 0x6e
#define M5PM1_REG_DEVICE_ID 0x00
#define M5PM1_REG_PWR_CFG 0x06
#define M5PM1_REG_HOLD_CFG 0x07
#define M5PM1_REG_GPIO_MODE 0x10
#define M5PM1_REG_GPIO_OUT 0x11
#define M5PM1_REG_GPIO_DRV 0x13
#define M5PM1_REG_GPIO_FUNC0 0x16
#define M5PM1_REG_BAT_L 0x22
#define M5PM1_REG_IRQ_STATUS1 0x40
#define M5PM1_REG_IRQ_STATUS2 0x41
#define M5PM1_REG_IRQ_STATUS3 0x42
#define M5PM1_REG_IRQ_MASK1 0x43
#define M5PM1_REG_IRQ_MASK2 0x44
#define M5PM1_REG_IRQ_MASK3 0x45
#define M5PM1_PWR_CFG_LDO_EN BIT(2)
#define M5PM1_PWR_CFG_LED_CTRL BIT(4)
#define M5PM1_HOLD_CFG_LDO_HOLD BIT(5)
#define M5PM1_GPIO2_L3B_EN BIT(2)
#define M5PM1_IRQ_SYS_5VIN_INSERT BIT(0)
#define M5PM1_IRQ_SYS_5VIN_REMOVE BIT(1)

static i2c_master_dev_handle_t g_pmic = NULL;

static bool pmic_read(uint8_t reg, uint8_t *val) {
    if (!g_pmic) return false;
    return i2c_master_transmit_receive(g_pmic, &reg, 1, val, 1, 100) == ESP_OK;
}

static bool pmic_write(uint8_t reg, uint8_t val) {
    if (!g_pmic) return false;
    uint8_t d[] = {reg, val};
    return i2c_master_transmit(g_pmic, d, 2, 100) == ESP_OK;
}

static bool pmic_find(i2c_port_t port, gpio_num_t sda, gpio_num_t scl) {
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bc = {};
    bc.i2c_port = port;
    bc.sda_io_num = sda;
    bc.scl_io_num = scl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7;
    bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = M5PM1_ADDR;
    dc.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(bus, &dc, &g_pmic) != ESP_OK) return false;
    uint8_t id;
    return pmic_read(M5PM1_REG_DEVICE_ID, &id);
}

static void pmic_init(void) {
    const struct { i2c_port_t p; gpio_num_t s; gpio_num_t c; } t[] = {
        {I2C_NUM_1, GPIO_NUM_47, GPIO_NUM_48},
        {I2C_NUM_1, GPIO_NUM_48, GPIO_NUM_47},
        {I2C_NUM_0, GPIO_NUM_47, GPIO_NUM_48},
        {I2C_NUM_0, GPIO_NUM_48, GPIO_NUM_47},
    };
    for (int i = 0; i < 4; i++) {
        if (pmic_find(t[i].p, t[i].s, t[i].c)) {
            ESP_LOGI(TAG, "PMIC found port=%d", t[i].p);
            break;
        }
    }
    if (!g_pmic) { ESP_LOGW(TAG, "PMIC not found"); return; }

    uint8_t v;
    pmic_read(M5PM1_REG_PWR_CFG, &v);
    pmic_write(M5PM1_REG_PWR_CFG, (v | M5PM1_PWR_CFG_LDO_EN) & ~M5PM1_PWR_CFG_LED_CTRL);
    pmic_read(M5PM1_REG_HOLD_CFG, &v);
    pmic_write(M5PM1_REG_HOLD_CFG, v | M5PM1_HOLD_CFG_LDO_HOLD);

    pmic_read(M5PM1_REG_GPIO_FUNC0, &v); v &= ~M5PM1_GPIO2_L3B_EN; pmic_write(M5PM1_REG_GPIO_FUNC0, v);
    pmic_read(M5PM1_REG_GPIO_MODE, &v);  v |= M5PM1_GPIO2_L3B_EN;  pmic_write(M5PM1_REG_GPIO_MODE, v);
    pmic_read(M5PM1_REG_GPIO_DRV, &v);   v &= ~M5PM1_GPIO2_L3B_EN; pmic_write(M5PM1_REG_GPIO_DRV, v);
    pmic_read(M5PM1_REG_GPIO_OUT, &v);   v |= M5PM1_GPIO2_L3B_EN;  pmic_write(M5PM1_REG_GPIO_OUT, v);

    pmic_write(M5PM1_REG_IRQ_MASK1, 0x1F);
    pmic_write(M5PM1_REG_IRQ_MASK3, 0x07);
    pmic_write(M5PM1_REG_IRQ_MASK2, 0x3F & ~(M5PM1_IRQ_SYS_5VIN_INSERT | M5PM1_IRQ_SYS_5VIN_REMOVE));
    ESP_LOGI(TAG, "PMIC ready");
}

static int pmic_battery_pct(void) {
    if (!g_pmic) return -1;
    uint8_t d[2];
    pmic_read(M5PM1_REG_BAT_L, &d[0]);
    pmic_read(M5PM1_REG_BAT_L + 1, &d[1]);
    int mv = d[0] | (d[1] << 8);
    int pct = (mv - 3300) * 100 / (4150 - 3350);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

// ── Read auth server URL from NVS ───────────────────────

static void get_server_url(char *buf, size_t buflen) {
    bool valid = false;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = buflen;
        if (nvs_get_str(nvs, "auth_server_url", buf, &len) == ESP_OK && len > 0) {
            valid = true;
        }
        nvs_close(nvs);
    }
    if (!valid) {
        strncpy(buf, CONFIG_AUTH_SERVER_URL, buflen - 1);
    }
    buf[buflen - 1] = '\0';
}

// ── Device registration ────────────────────────────────

static EspNetwork g_network;

static bool device_ensure_registered(const char *server_url) {
    char mac[18];
    auth_client_get_mac(mac);
    char url[256];

    // Check if already registered
    snprintf(url, sizeof(url), "%s/api/device/status?mac=%s", server_url, mac);
    auto http = g_network.CreateHttp(0);
    http->SetTimeout(5000);
    if (http->Open("GET", url)) {
        int st = http->GetStatusCode();
        if (st == 200) {
            std::string body = http->ReadAll();
            cJSON *r = cJSON_Parse(body.c_str());
            if (r) {
                cJSON *reg = cJSON_GetObjectItem(r, "registered");
                if (reg && cJSON_IsTrue(reg)) { cJSON_Delete(r); http->Close(); return true; }
                cJSON_Delete(r);
            }
        }
        ESP_LOGI(TAG, "Device check: status=%d", st);
    }
    http->Close();

    // Register device
    ESP_LOGI(TAG, "Device registering...");
    display_show_connecting();
    snprintf(url, sizeof(url), "%s/api/device/register", server_url);
    auto http2 = g_network.CreateHttp(0);
    http2->SetTimeout(5000);
    http2->SetHeader("Content-Type", "application/json");
    char post_body[128];
    snprintf(post_body, sizeof(post_body), "{\"mac\":\"%s\"}", mac);
    http2->SetContent(std::move(std::string(post_body)));
    int st = 0;
    char code[8] = {0};
    if (http2->Open("POST", url)) {
        st = http2->GetStatusCode();
        if (st == 200) {
            std::string body = http2->ReadAll();
            cJSON *r = cJSON_Parse(body.c_str());
            if (r) {
                cJSON *jc = cJSON_GetObjectItem(r, "code");
                if (jc && jc->valuestring) {
                    strncpy(code, jc->valuestring, sizeof(code) - 1);
                    ESP_LOGI(TAG, "Reg code: %s", code);
                }
                cJSON_Delete(r);
            }
        }
    }
    http2->Close();

    if (code[0]) {
        display_show_code(code, "RegCode", 300);
        // Poll for admin approval
        int64_t start = esp_timer_get_time();
        while ((esp_timer_get_time() - start) < 300 * 1000000LL) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            snprintf(url, sizeof(url), "%s/api/device/status?mac=%s", server_url, mac);
            auto poll = g_network.CreateHttp(0);
            poll->SetTimeout(5000);
            if (poll->Open("GET", url) && poll->GetStatusCode() == 200) {
                std::string body = poll->ReadAll();
                cJSON *r = cJSON_Parse(body.c_str());
                if (r) {
                    cJSON *reg = cJSON_GetObjectItem(r, "registered");
                    if (reg && cJSON_IsTrue(reg)) {
                        cJSON_Delete(r); poll->Close();
                        display_show_result(AUTH_STATE_APPROVED);
                        vTaskDelay(pdMS_TO_TICKS(2000));
                        return true;
                    }
                    cJSON_Delete(r);
                }
            }
            poll->Close();
        }
    } else {
        char err[64];
        snprintf(err, sizeof(err), "reg err:%d", st);
        display_show_error(err);
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
    return false;
}

// ── Battery refresh timer ───────────────────────────────

static void battery_timer_cb(void*) {
    int pct = pmic_battery_pct();
    if (pct >= 0) display_set_battery(pct, false);
}

// ── Re-enter config mode ────────────────────────────────

static void reenter_config(WifiManager &wifi) {
    ESP_LOGI(TAG, "Re-entering config mode...");
    wifi.StartConfigAp();
    display_set_config_mode(true);
    display_show_wifi_config(wifi.GetApSsid().c_str());
}

// ── Main ────────────────────────────────────────────────

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== AuthStick booting ===");
    pmic_init();
    display_init();
    display_show_connecting();

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    esp_timer_handle_t bat_timer;
    esp_timer_create_args_t bat_args = {};
    bat_args.callback = battery_timer_cb;
    bat_args.name = "bat";
    bat_args.skip_unhandled_events = true;
    esp_timer_create(&bat_args, &bat_timer);
    esp_timer_start_periodic(bat_timer, 30 * 1000000ULL);
    battery_timer_cb(nullptr);

    // ── WiFi ────────────────────────────────────────────
    auto& wifi = WifiManager::GetInstance();
    WifiManagerConfig wcfg;
    wcfg.ssid_prefix = "AuthStick";
    wifi.Initialize(wcfg);

    EventGroupHandle_t wifi_evt = xEventGroupCreate();
    wifi.SetEventCallback([&](WifiEvent event, const std::string& data) {
        switch (event) {
        case WifiEvent::Connected:
            ESP_LOGI(TAG, "WiFi connected: %s RSSI=%d", wifi.GetSsid().c_str(), wifi.GetRssi());
            display_set_wifi_connected(true);
            display_set_wifi(wifi.GetRssi());
            xEventGroupSetBits(wifi_evt, BIT0);
            break;
        case WifiEvent::Disconnected:
            display_set_wifi_connected(false);
            break;
        case WifiEvent::ConfigModeEnter:
            ESP_LOGI(TAG, "Config mode: AP=%s", wifi.GetApSsid().c_str());
            display_set_config_mode(true);
            display_show_wifi_config(wifi.GetApSsid().c_str());
            break;
        case WifiEvent::ConfigModeExit:
            ESP_LOGI(TAG, "Config done, restarting...");
            esp_restart();
            break;
        default: break;
        }
    });

    // ── Connect to WiFi ─────────────────────────────────
    auto& ssid_mgr = SsidManager::GetInstance();
    if (ssid_mgr.GetSsidList().empty()) {
        ESP_LOGI(TAG, "No saved WiFi, entering config mode...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        wifi.StartConfigAp();
    } else {
        wifi.StartStation();
        EventBits_t bits = xEventGroupWaitBits(wifi_evt, BIT0, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
        if (!wifi.IsConnected()) {
            ESP_LOGI(TAG, "WiFi connect failed, starting config AP...");
            wifi.StartConfigAp();
        }
    }

    // Wait for WiFi connection
    while (!wifi.IsConnected()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    ESP_LOGI(TAG, "WiFi connected!");

    // ── TEST: just do one GET request ──
    char server_url[128] = {0};
    get_server_url(server_url, sizeof(server_url));
    auth_client_init(server_url);
    button_init();
    display_show_connecting();

    // ── Registration in main task ──
    auth_client_init(server_url);
    vTaskDelay(pdMS_TO_TICKS(3000));

    char reg_url[128];
    get_server_url(reg_url, sizeof(reg_url));
    bool reg_ok = false;
    while (!reg_ok) {
        if (device_ensure_registered(reg_url)) {
            reg_ok = true;
            break;
        }
        ESP_LOGW("reg", "Registration failed, retrying...");
        vTaskDelay(pdMS_TO_TICKS(5000));
        get_server_url(reg_url, sizeof(reg_url));
        auth_client_init(reg_url);
    }

    // ── Main loop ───────────────────────────────────────
    display_show_idle();
    auth_pending_code_t pending[4];
    int code_count = 0;
    int64_t last_poll = 0, result_shown_at = 0;
    int countdown = 0;
    TickType_t last_tick = xTaskGetTickCount();

    while (1) {
        TickType_t now_tick = xTaskGetTickCount();
        int elapsed_ms = (now_tick - last_tick) * portTICK_PERIOD_MS;
        last_tick = now_tick;

        button_event_t btn = button_poll();
        if (code_count > 0 && btn == BTN_A_SHORT) {
            ESP_LOGI(TAG, "APPROVE %s", pending[0].code);
            if (auth_client_approve(pending[0].code))
                display_show_result(AUTH_STATE_APPROVED);
            else
                display_show_error("\xe6\x89\xb9\xe5\x87\x86\xe5\xa4\xb1\xe8\xb4\xa5");
            code_count = 0; result_shown_at = esp_timer_get_time();
        } else if (code_count > 0 && btn == BTN_B_SHORT) {
            ESP_LOGI(TAG, "DENY %s", pending[0].code);
            if (auth_client_deny(pending[0].code))
                display_show_result(AUTH_STATE_DENIED);
            else
                display_show_error("\xe6\x8b\x92\xe7\xbb\x9d\xe5\xa4\xb1\xe8\xb4\xa5");
            code_count = 0; result_shown_at = esp_timer_get_time();
        }
        if (result_shown_at > 0 && (esp_timer_get_time() - result_shown_at) > RESULT_SHOW_MS * 1000LL) {
            display_show_idle(); result_shown_at = 0;
        }

        if (code_count > 0 && countdown > 0) {
            countdown -= elapsed_ms / 1000;
            if (countdown <= 0) { code_count = 0; display_show_idle(); }
            else display_update_countdown(countdown);
        }

        if (code_count == 0 && result_shown_at == 0 &&
            (esp_timer_get_time() - last_poll) > POLL_INTERVAL_MS * 1000LL) {
            last_poll = esp_timer_get_time();
            int n = auth_client_poll(pending, 4);
            if (n > 0) {
                code_count = 1; countdown = pending[0].expires_in;
                display_show_code(pending[0].code, pending[0].service_name, countdown);
                ESP_LOGI(TAG, "Got code: %s", pending[0].code);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
