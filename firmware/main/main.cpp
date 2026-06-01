#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "auth_client.h"
#include "display.h"
#include "button.h"
#include "driver/i2c_master.h"
#include "wifi_manager.h"
#include "ssid_manager.h"
#include "esp_network.h"

static const char *TAG = "main";

#define POLL_INTERVAL_MS    30000   // 30s between auth code polls
#define RESULT_SHOW_MS      2000
#define CODE_TTL_US         300000000LL  // 5 min
#define POLL_APPROVAL_US    5000000LL    // 5s between approval checks
#define RETRY_DELAY_US      5000000LL    // 5s retry

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

// ── State machine ───────────────────────────────────────

enum DevState {
    S_BOOTING,          // waiting for WiFi to connect
    S_NO_WIFI,          // config AP active, waiting for user config
    S_CHECKING,         // HTTP GET /api/device/status  (1 step)
    S_REGISTERING,      // HTTP POST /api/device/register (1 step)
    S_WAIT_APPROVAL,    // code displayed, polling for admin
    S_READY,            // registered, polling for auth codes
    S_ERROR,            // transient error, retry after delay
};

static DevState g_state = S_BOOTING;
static int64_t g_state_deadline;   // when to advance (timeout/retry)
static int64_t g_next_poll;        // next poll time (approval or auth codes)
static int64_t g_code_expires_at;  // auth code expiry timestamp
static int64_t g_token_rotate_at; // next token rotation time
static bool g_code_pending;       // new code to display after HTTP done
static char g_pending_code[8];
static char g_pending_name[32];
static int g_pending_expires;
static bool g_banned = false;
static bool g_pending_banned;      // banned detected by poll task
static bool g_pending_token_err;   // token invalid, need re-register
static char g_reg_code[8];         // current registration code
static EspNetwork g_network;
static char g_server_url[128];
static char g_mac[18];
static char g_devname[64];
static char g_device_token[64];

static void set_state(DevState s, int64_t deadline_us) {
    g_state = s;
    g_state_deadline = esp_timer_get_time() + deadline_us;
    ESP_LOGI(TAG, "State -> %d deadline=%lld", s, deadline_us / 1000);
}

// ── PMIC ─────────────────────────────────────────────────

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
    bc.i2c_port = port; bc.sda_io_num = sda; bc.scl_io_num = scl;
    bc.clk_source = I2C_CLK_SRC_DEFAULT;
    bc.glitch_ignore_cnt = 7; bc.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bc, &bus) != ESP_OK) return false;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = M5PM1_ADDR; dc.scl_speed_hz = 100000;
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
        if (pmic_find(t[i].p, t[i].s, t[i].c)) { ESP_LOGI(TAG, "PMIC found"); break; }
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

// ── Auth helpers ────────────────────────────────────────

static void get_server_url(char *buf, size_t buflen) {
    bool valid = false;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = buflen;
        if (nvs_get_str(nvs, "auth_server_url", buf, &len) == ESP_OK && len > 0) valid = true;
        nvs_close(nvs);
    }
    if (!valid) strncpy(buf, CONFIG_AUTH_SERVER_URL, buflen - 1);
    buf[buflen - 1] = '\0';
}

// ── State machine step (called once per main loop iteration) ──

// ── Background poll task ─────────────────────────────────
// Runs HTTP polling in separate task to avoid lock contention with LVGL main task

static void bg_poll_task(void *arg) {
    ESP_LOGI(TAG, "Poll task started");
    vTaskDelay(pdMS_TO_TICKS(3000));
    while (1) {
        if (g_banned || g_state != S_READY || g_code_pending) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!g_device_token[0]) {
            ESP_LOGW(TAG, "No device token — re-register required");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }
        if (g_code_expires_at > 0 && esp_timer_get_time() < g_code_expires_at) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        // Rotate token every 24 hours
        if (esp_timer_get_time() > g_token_rotate_at) {
            char rt_url[256]; char rt_body[128];
            snprintf(rt_url, sizeof(rt_url), "%s/api/stick/rotate-token", g_server_url);
            snprintf(rt_body, sizeof(rt_body), "{\"token\":\"%s\",\"mac\":\"%s\"}", g_device_token, g_mac);
            auto rt = g_network.CreateHttp(0);
            rt->SetTimeout(3000);
            rt->SetHeader("Content-Type", "application/json");
            rt->SetContent(std::move(std::string(rt_body)));
            if (rt->Open("POST", rt_url) && rt->GetStatusCode() == 200) {
                std::string resp = rt->ReadAll();
                cJSON *r = cJSON_Parse(resp.c_str());
                if (r) {
                    cJSON *jt = cJSON_GetObjectItem(r, "device_token");
                    if (jt && jt->valuestring && jt->valuestring[0]) {
                        strncpy(g_device_token, jt->valuestring, sizeof(g_device_token)-1);
                        nvs_handle_t nvs; nvs_open("wifi", NVS_READWRITE, &nvs);
                        nvs_set_str(nvs, "dev_token", g_device_token);
                        g_token_rotate_at = esp_timer_get_time() + 86400000000LL;
                        nvs_set_u64(nvs, "token_rot", (uint64_t)g_token_rotate_at);
                        nvs_commit(nvs); nvs_close(nvs);
                        ESP_LOGI(TAG, "Token rotated");
                    }
                    cJSON_Delete(r);
                }
            }
            rt->Close();
            g_token_rotate_at = esp_timer_get_time() + 86400000000LL;  // 24h
        }

        char url[256];
        snprintf(url, sizeof(url), "%s/api/stick/generate", g_server_url);
        char body[128];
        snprintf(body, sizeof(body), "{\"token\":\"%s\",\"mac\":\"%s\"}", g_device_token, g_mac);
        auto http = g_network.CreateHttp(0);
        http->SetTimeout(3000);
        http->SetHeader("Content-Type", "application/json");
        http->SetContent(std::move(std::string(body)));
        if (http->Open("POST", url) && http->GetStatusCode() == 200) {
            std::string resp = http->ReadAll();
            cJSON *r = cJSON_Parse(resp.c_str());
            if (r) {
                if (cJSON_IsTrue(cJSON_GetObjectItem(r, "banned"))) {
                    cJSON *dn = cJSON_GetObjectItem(r, "device_name");
                    if (dn && cJSON_IsString(dn)) strncpy(g_devname, dn->valuestring, sizeof(g_devname)-1);
                    g_pending_banned = true;
                } else {
                    cJSON *dn = cJSON_GetObjectItem(r, "device_name");
                    if (dn && cJSON_IsString(dn)) strncpy(g_devname, dn->valuestring, sizeof(g_devname)-1);
                    cJSON *jc = cJSON_GetObjectItem(r, "code");
                    cJSON *je = cJSON_GetObjectItem(r, "expires_in");
                    if (jc && jc->valuestring && je && je->valueint > 0) {
                        strncpy(g_pending_code, jc->valuestring, sizeof(g_pending_code)-1);
                        g_pending_expires = je->valueint;
                        strncpy(g_pending_name, g_devname[0] ? g_devname : "AuthStick", sizeof(g_pending_name)-1);
                        g_code_pending = true;
                        ESP_LOGI(TAG, "Auth code: %s", jc->valuestring);
                    }
                }
                cJSON_Delete(r);
            }
        } else {
            int st = http->GetStatusCode();
            ESP_LOGW(TAG, "Poll failed, status=%d, token=%s", st, g_device_token);
            if (st == 403) g_pending_token_err = true;
        }
        http->Close();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

static void sm_tick(void) {
    int64_t now = esp_timer_get_time();

    switch (g_state) {

    case S_BOOTING:
        // Just wait until network transitions us out
        break;

    case S_NO_WIFI:
        // AP mode active, nothing to do (user configures via captive portal)
        break;

    case S_CHECKING: {
        char url[256];
        snprintf(url, sizeof(url), "%s/api/device/status?mac=%s", g_server_url, g_mac);
        auto http = g_network.CreateHttp(0);
        http->SetTimeout(3000);
        bool already = false, banned = false;
        if (http->Open("GET", url) && http->GetStatusCode() == 200) {
            std::string body = http->ReadAll();
            cJSON *r = cJSON_Parse(body.c_str());
            if (r) {
                if (cJSON_IsTrue(cJSON_GetObjectItem(r, "registered"))) already = true;
                if (cJSON_IsTrue(cJSON_GetObjectItem(r, "banned"))) banned = true;
                cJSON *nm = cJSON_GetObjectItem(r, "name");
                if (nm && cJSON_IsString(nm)) strncpy(g_devname, nm->valuestring, sizeof(g_devname)-1);
                cJSON_Delete(r);
            }
        }
        http->Close();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (banned) {
            display_show_banned(g_mac, g_devname[0] ? g_devname : g_mac);
            g_banned = true;
            vTaskDelay(pdMS_TO_TICKS(3000));
            break;
        }
        if (already) { set_state(S_READY, CODE_TTL_US); display_show_idle(); }
        else set_state(S_REGISTERING, 0);
        break;
    }

    case S_REGISTERING: {
        char url[256];
        snprintf(url, sizeof(url), "%s/api/device/register", g_server_url);
        auto http = g_network.CreateHttp(0);
        http->SetTimeout(3000);
        http->SetHeader("Content-Type", "application/json");
        char body[128]; snprintf(body, sizeof(body), "{\"mac\":\"%s\"}", g_mac);
        http->SetContent(std::move(std::string(body)));
        memset(g_reg_code, 0, sizeof(g_reg_code));
        bool got_code = false;
        if (http->Open("POST", url) && http->GetStatusCode() == 200) {
            std::string resp = http->ReadAll();
            cJSON *r = cJSON_Parse(resp.c_str());
            if (r) {
                cJSON *jc = cJSON_GetObjectItem(r, "code");
                if (jc && jc->valuestring) {
                    strncpy(g_reg_code, jc->valuestring, sizeof(g_reg_code)-1);
                    got_code = true;
                }
                cJSON_Delete(r);
            }
        }
        http->Close();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (got_code) {
            display_show_code(g_reg_code, "RegCode", 300);
            // Bind device token to registration code
            if (g_device_token[0]) {
                char bd_url[256];
                snprintf(bd_url, sizeof(bd_url), "%s/api/device/bind-token", g_server_url);
                auto bd = g_network.CreateHttp(0);
                bd->SetTimeout(3000);
                bd->SetHeader("Content-Type", "application/json");
                char bd_body[320];
                snprintf(bd_body, sizeof(bd_body), "{\"mac\":\"%s\",\"code\":\"%s\",\"device_token\":\"%s\"}", g_mac, g_reg_code, g_device_token);
                bd->SetContent(std::move(std::string(bd_body)));
                if (bd->Open("POST", bd_url) && bd->GetStatusCode() == 200) {
                    ESP_LOGI(TAG, "Token bound to registration");
                } else {
                    ESP_LOGW(TAG, "Bind-token failed, status=%d", bd->GetStatusCode());
                }
                bd->Close();
            }
            set_state(S_WAIT_APPROVAL, CODE_TTL_US);
            g_next_poll = now + POLL_APPROVAL_US;
            ESP_LOGI(TAG, "Reg code: %s", g_reg_code);
        } else {
            display_show_error("reg err");
            set_state(S_ERROR, RETRY_DELAY_US);
        }
        break;
    }

    case S_WAIT_APPROVAL:
        if (now > g_state_deadline) {
            // Code expired, get a new one
            set_state(S_REGISTERING, 0);
            break;
        }
        if (now >= g_next_poll) {
            g_next_poll = now + POLL_APPROVAL_US;
            char url[256];
            snprintf(url, sizeof(url), "%s/api/device/status?mac=%s", g_server_url, g_mac);
            auto http = g_network.CreateHttp(0);
            http->SetTimeout(3000);
            bool approved = false;
            if (http->Open("GET", url) && http->GetStatusCode() == 200) {
                std::string body = http->ReadAll();
                cJSON *r = cJSON_Parse(body.c_str());
                if (r && cJSON_IsTrue(cJSON_GetObjectItem(r, "registered"))) approved = true;
                if (r) cJSON_Delete(r);
            }
            http->Close();
            vTaskDelay(pdMS_TO_TICKS(100));
            if (approved) {
                display_show_result(AUTH_STATE_APPROVED);
                vTaskDelay(pdMS_TO_TICKS(2000));
                set_state(S_READY, 0);
            }
        }
        break;

    case S_READY:
        // Polling handled by background task — nothing to do here
        break;

    case S_ERROR:
        if (now >= g_state_deadline) {
            get_server_url(g_server_url, sizeof(g_server_url));
            auth_client_init(g_server_url);
            set_state(S_CHECKING, 0);
        }
        break;
    }
}

// ── Battery timer ────────────────────────────────────────

static void battery_timer_cb(void*) {
    int pct = pmic_battery_pct();
    if (pct >= 0) display_set_battery(pct, false);
}

// ── Main ────────────────────────────────────────────────

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== AuthStick booting ===");
    pmic_init();
    nvs_flash_init();
    display_init();
    display_show_connecting();
    button_init();
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
    bool need_station = false;
    wifi.SetEventCallback([&](WifiEvent event, const std::string& data) {
        switch (event) {
        case WifiEvent::Connected:
            ESP_LOGI(TAG, "WiFi connected: %s", wifi.GetSsid().c_str());
            display_set_wifi_connected(true);
            display_set_wifi(wifi.GetRssi());
            xEventGroupSetBits(wifi_evt, BIT0);
            break;
        case WifiEvent::Disconnected:
            display_set_wifi_connected(false);
            break;
        case WifiEvent::ConfigModeEnter:
            ESP_LOGI(TAG, "Config mode: %s", wifi.GetApSsid().c_str());
            display_set_config_mode(true);
            display_show_wifi_config(wifi.GetApSsid().c_str());
            break;
        case WifiEvent::ConfigModeExit:
            ESP_LOGI(TAG, "Config done, starting station...");
            need_station = true;
            xEventGroupSetBits(wifi_evt, BIT1);
            break;
        default: break;
        }
    });

    // ── Connect (with button handling during wait) ─────
    auto handle_buttons = []() {
        button_event_t btn = button_poll();
        bool has_ov = display_has_overlay();
        overlay_page_t ov = display_get_overlay();
        if (ov == OVERLAY_MENU && btn == BTN_B_SHORT) display_menu_next();
        else if (ov == OVERLAY_MENU && (btn == BTN_A_SHORT || btn == BTN_A_LONG)) display_menu_select();
        else if (ov == OVERLAY_RESET_CONFIRM) {
            if (btn == BTN_A_SHORT || btn == BTN_A_LONG) { nvs_flash_erase(); esp_restart(); }
            else if (btn == BTN_B_SHORT || btn == BTN_B_LONG) display_show_menu();
        } else if (ov == OVERLAY_USAGE || ov == OVERLAY_ABOUT) {
            if (btn == BTN_A_SHORT || btn == BTN_B_SHORT || btn == BTN_B_LONG) display_show_menu();
        } else if (!has_ov && btn == BTN_A_SHORT) {
            static bool scr = false; scr = !scr; display_set_backlight(!scr);
        } else if (!has_ov && (btn == BTN_B_SHORT || btn == BTN_B_LONG)) display_show_menu();
    };

    auto& ssid_mgr = SsidManager::GetInstance();
    if (ssid_mgr.GetSsidList().empty()) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        wifi.StartConfigAp();
    } else {
        wifi.StartStation();
        for (int i = 0; i < 60 && !wifi.IsConnected(); i++) {
            EventBits_t bits = xEventGroupWaitBits(wifi_evt, BIT0, pdFALSE, pdFALSE, pdMS_TO_TICKS(500));
            handle_buttons();
        }
        if (!wifi.IsConnected()) wifi.StartConfigAp();
    }
    while (!wifi.IsConnected()) {
        if (need_station) { need_station = false; wifi.StartStation(); }
        vTaskDelay(pdMS_TO_TICKS(100));
        handle_buttons();
    }
    ESP_LOGI(TAG, "WiFi connected!");
    if (!display_has_overlay()) display_show_connecting();

    // Sync time via SNTP
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    int retry = 0;
    while (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < 50) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    setenv("TZ", "CST-8", 1); tzset();
    ESP_LOGI(TAG, "SNTP sync %s", esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED ? "OK" : "FAIL");

    // ── Init ────────────────────────────────────────────
    get_server_url(g_server_url, sizeof(g_server_url));
    // Load or generate device token
    g_device_token[0] = 0;
    nvs_handle_t nvs;
    if (nvs_open("wifi", NVS_READWRITE, &nvs) == ESP_OK) {
        size_t len = sizeof(g_device_token);
        if (nvs_get_str(nvs, "dev_token", g_device_token, &len) != ESP_OK || !g_device_token[0]) {
            // Generate random token on first boot
            uint32_t r[4];
            for (int i = 0; i < 4; i++) r[i] = esp_random();
            snprintf(g_device_token, sizeof(g_device_token), "%08lx%08lx%08lx%08lx", (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2], (unsigned long)r[3]);
            nvs_set_str(nvs, "dev_token", g_device_token);
            nvs_commit(nvs);
            ESP_LOGI(TAG, "Device token generated");
        }
        // Load last token rotation time
        uint64_t rot = 0;
        if (nvs_get_u64(nvs, "token_rot", &rot) == ESP_OK) g_token_rotate_at = (int64_t)rot;
        nvs_close(nvs);
    }
    if (!g_token_rotate_at) g_token_rotate_at = esp_timer_get_time() + 86400000000LL;
    auth_client_init(g_server_url);
    auth_client_get_mac(g_mac);
    display_set_mac(g_mac);
    g_devname[0] = 0;
    xTaskCreatePinnedToCore(bg_poll_task, "bg_poll", 8192, NULL, 5, NULL, 0);
    vTaskPrioritySet(nullptr, 10);

    // Enter registration flow
    set_state(S_CHECKING, 0);

    // ── Main event loop ─────────────────────────────────
    bool idle_shown = false;

    while (1) {
        // Run state machine (one step, non-blocking)
        if (!g_banned) sm_tick();

        // Show idle once registered
        if (g_state == S_READY && !idle_shown) {
            idle_shown = true;
            display_show_idle();
        }

        // Handle banned detected by background poll task
        if (g_pending_banned && !display_has_overlay()) {
            g_pending_banned = false;
            g_banned = true;
            display_show_banned(g_mac, g_devname[0] ? g_devname : g_mac);
        }

        // Handle token error — skip if overlay active
        if (g_pending_token_err && !display_has_overlay()) {
            g_pending_token_err = false;
            display_show_token_error();
            g_banned = false;
        }

        // Show pending auth code — skip if overlay active
        if (g_code_pending && !display_has_overlay()) {
            g_code_pending = false;
            g_code_expires_at = esp_timer_get_time() + g_pending_expires * 1000000LL;
            display_show_code(g_pending_code, g_pending_name, g_pending_expires);
        }

        // ── Button handling ──────────────────────────────
        button_event_t btn = button_poll();
        bool has_overlay = display_has_overlay();
        overlay_page_t ov = display_get_overlay();

        if (ov == OVERLAY_MENU && btn == BTN_B_SHORT) {
            display_menu_next();
        } else if (ov == OVERLAY_MENU && (btn == BTN_A_SHORT || btn == BTN_A_LONG)) {
            display_menu_select();
        } else if (ov == OVERLAY_RESET_CONFIRM) {
            if (btn == BTN_A_SHORT || btn == BTN_A_LONG) {
                ESP_LOGI(TAG, "Factory reset confirmed");
                nvs_flash_erase();
                esp_restart();
            } else if (btn == BTN_B_SHORT || btn == BTN_B_LONG) {
                display_show_menu();
            }
        } else if (ov == OVERLAY_USAGE || ov == OVERLAY_ABOUT) {
            if (btn == BTN_A_SHORT || btn == BTN_B_SHORT || btn == BTN_B_LONG)
                display_show_menu();
        } else if (!has_overlay && btn == BTN_A_SHORT) {
            static bool screen_off = false;
            screen_off = !screen_off;
            display_set_backlight(!screen_off);
        } else if (!has_overlay && (btn == BTN_B_SHORT || btn == BTN_B_LONG)) {
            display_show_menu();
        }

        // Code expiry — clear timer, poll task will refresh
        if (g_code_expires_at > 0 && esp_timer_get_time() > g_code_expires_at) {
            g_code_expires_at = 0;
        }

        // ── Auto sleep / wake ──────────────────────────
        static bool asleep = false;
        static int64_t last_act = 0;
        if (btn != BTN_NONE) last_act = esp_timer_get_time();
        if (asleep && btn != BTN_NONE) {
            asleep = false; display_set_backlight(true);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;  // skip action on wake-up press
        }
        if (!asleep && esp_timer_get_time() - last_act > 30000000LL) {
            if (btn == BTN_NONE) { asleep = true; display_set_backlight(false); }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
