# AuthStick Project

ESP32-S3 firmware + web flash tool + Python auth backend.
StickS3 firmware development covered by `sticks3-dev` skill.

## Quick Start

```bash
# Firmware build + merge + sha256 (run build.sh)
cd firmware && bash build.sh

# Web-flash dev server (port 8999)
cd web-flash && python3 -m http.server 8999

# Auth backend server (port 8998)
cd server && pip install -r requirements.txt && python server.py
```

## Ports

| Service | Port |
|---------|------|
| Web-flash UI | 8999 |
| Auth backend | 8998 |

## Project Structure

- `firmware/` — ESP32-S3 firmware (ESP-IDF 5.5, LVGL 9.5)
- `web-flash/` — Browser-based flashing UI (esptool-js)
- `server/` — Python auth backend server

## Key Files

- `firmware/main/main.cpp` — State machine, button handling, main loop, poll task
- `firmware/components/display/display.c` — All LVGL UI pages and overlays
- `firmware/components/display/display.h` — UI function declarations
- `firmware/components/auth_client/auth_client.c` — HTTP client, MAC reading

## UI Architecture

- Base page + overlays. `close_base_page()` + `make_base_page()` for page switches. `close_overlay()` + `make_overlay_panel()` for overlays.
- `g_page_base` — base content area (W x H-BAR_H, aligned BOTTOM_MID)
- `g_status_label` — persistent top bar text (never deleted)
- `g_label_hint` — bottom key hints (A:xxx B:xxx)
- `display_has_overlay()` checks `g_overlay_page != OVERLAY_NONE`
- Button A short on non-overlay pages = toggle backlight
- Menu overlay must ALWAYS be top layer. Base page updates must check `!display_has_overlay()` before rendering.

## Security Model

### Token System
- Device generates 128-bit `device_token` via `esp_random()` on first boot, stored in NVS.
- Token never appears in any server response. Only flows device→server.
- Token bound to registration code via `POST /api/device/bind-token {mac, code, device_token}`.
- `register_verify` requires token bound before allowing verification.
- MAC alone cannot get token. Registration code is one-time credential from device screen.
- Token auto-rotates every 24h via `POST /api/stick/rotate-token` (old token proves identity).
- Admin "重置令牌" clears server-side token, device re-registers on reboot.

### Verification Codes
- Device calls `POST /api/stick/generate {token, mac}` — POST body, not GET query.
- Server verifies both token AND MAC match before generating code.
- 6-digit codes, 5-minute TTL, generated server-side.

## HTTP Client Architecture (CRITICAL)

- **DO NOT use `esp_http_client`** alongside the custom lwip stack from `78__esp-wifi-connect`.
  Causes spinlock assertions, connection resets, and `_lock_close` crashes.
- **ALWAYS use `g_network.CreateHttp()`** — the same `EspNetwork` from `78__esp-ml307`.
- HTTP polling runs in a **separate FreeRTOS task** (`bg_poll_task`, priority 5, pinned Core 0).
  This avoids lock contention with LVGL main task (priority 10).
- Results communicated via `g_code_pending`/`g_pending_banned` flags, consumed in main loop.
- `nvs_flash_init()` MUST run BEFORE `display_init()` (NVS needed for language persistence).

## Conventions

- Display functions MUST set `g_label_hint` on non-overlay pages.
- cJSON objects MUST be read completely before `cJSON_Delete()`.
- Use `t("中文", "English")` for ALL user-facing strings (i18n with `g_lang_en` switch).
- Language toggle saves to NVS. `MENU_BACK` re-renders code/WiFi pages on language change.
- Font: `BUILTIN_TEXT_FONT` for Chinese/English, `font_digits_30_4` for verification codes.
- MAC format: uppercase hex with colons `XX:XX:XX:XX:XX:XX`.
- Auto-sleep: 30s no button → backlight off, any key wakes (wake press consumed).

## Gotchas

- `managed_components/` is gitignored — changes there are lost on rebuild.
- LVGL `transform_scale` byte order: RGB565 is big-endian, ARGB8888 is BGRA on LE ESP32.
- LVGL lock (`take_lock`/`give_lock`) is NOT recursive — don't call display functions from within locked context.
- `font_digits_30_4` needs `pad_bottom=6` to prevent LVGL clipping descender pixels.
- The `78__esp-wifi-connect` component's `GetSsid()` uses `ESP_MAC_WIFI_SOFTAP` (STA MAC + 1).
- Auto-merge in CMakeLists was removed — use `build.sh` for merge.
