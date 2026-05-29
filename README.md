# AuthStick

ESP32-S3 physical authentication terminal. 4-digit codes displayed on screen,
physical buttons for approve/deny. No LLM, no voice — pure hardware auth.

## Why

Typing verification codes or relying on TTS is slow and insecure. This device:
- Shows codes on screen, never spoken aloud
- Physical buttons for instant approve/deny
- Device MAC provides hardware identity
- WiFi provisioning via phone — no desktop needed
- Admin approval required for device registration

## Hardware

- M5Stack StickS3 (ESP32-S3-PICO-1-N8R8)
- 135x240 ST7789 display (LVGL)
- 2 physical buttons: A (front, GPIO11), B (side, GPIO12)
- [Official docs](https://docs.m5stack.com/en/core/StickS3)

### Pinout

| Function | GPIO | Notes |
|----------|------|-------|
| LCD MOSI | 39 | ST7789 SPI |
| LCD SCK  | 40 | ST7789 SPI |
| LCD DC   | 45 | RS/DC |
| LCD CS   | 41 | Chip select |
| LCD RST  | 21 | Reset |
| LCD BL   | 38 | Backlight |
| Button A | 11 | Front (批准) |
| Button B | 12 | Side (拒绝/配网) |

## Quick Start

```bash
# Server
cd server && pip install fastapi uvicorn
python3 server.py --port 8998

# Firmware (requires ESP-IDF v5.5)
cd firmware && idf.py build && idf.py flash

# Web Flash (no tools needed)
cd web-flash && python3 -m http.server 8900
# Open http://<ip>:8900 in Chrome/Edge
```

## Boot Flow

```
Power on → check NVS for WiFi
  ├─ Saved WiFi → connect → HTTP register device
  │   ├─ Registered → idle → poll for auth codes
  │   └─ Not registered → POST /api/device/register
  │       → display 6-digit code → admin approves at /admin
  │       → poll until approved → idle
  └─ No saved WiFi → AP mode (AuthStick-XXXX)
      → phone connects → captive portal at 192.168.4.1
      → 3 tabs: Wi-Fi | Auth Server | Advanced
      → configure WiFi + auth server URL → 完成退出 → reboot
```

## Captive Portal Tabs

| Tab | Description |
|-----|-------------|
| Wi-Fi 配置 | Scan/connect to WiFi, saved networks management |
| 认证服务 | Set auth server URL (default: sdkconfig CONFIG_AUTH_SERVER_URL) |
| 高级选项 | OTA URL, TX power, BSSID, sleep mode |

## WiFi + Auth Config Flow

1. Device boots without WiFi → AP `AuthStick-XXXX` appears
2. Phone connects to AP → captive portal at `192.168.4.1`
3. **Wi-Fi tab**: select and connect to WiFi
4. WiFi configured → done page → "配置认证服务" → Auth tab
5. **Auth tab**: set server URL (saved to NVS) → "完成退出"
6. Device reboots → connects WiFi → registers with auth server

## Architecture

```
AuthStick ──WiFi──→ Auth Server (:8998)
  │                    │
  ├─ GET /api/stick/pending?device=MAC    ← poll for auth requests
  ├─ POST /api/stick/approve              ← button A press
  ├─ POST /api/stick/deny                 ← button B press
  ├─ POST /api/device/register            ← first boot registration
  └─ Display shows code + countdown

Admin ──→ /admin                              ← verify new devices
User  ──→ /login                              ← web login page
```

## Server Endpoints

| Method | Path | Description |
|--------|------|-------------|
| GET | `/login` | Web login page |
| POST | `/api/code/create` | Generate 4-digit login code |
| GET | `/api/code/check?code=` | Poll for stick approval |
| GET | `/api/stick/pending?device=` | Stick polls for pending codes |
| POST | `/api/stick/approve` | Stick approves code `{code, device}` |
| POST | `/api/stick/deny` | Stick denies code `{code, device}` |
| POST | `/api/device/register` | Start device registration `{mac}` |
| GET | `/api/device/status?mac=` | Poll registration status |
| GET | `/admin` | Admin panel — device list + verify |
| POST | `/api/admin/verify-device` | Verify device `{code}` |
| GET | `/api/verify?token=` | Validate session |
| POST | `/api/logout` | Revoke session |

## Firmware Configuration

```bash
cd firmware
idf.py menuconfig
```

Key config:
- `CONFIG_AUTH_SERVER_URL` — default auth server (sdkconfig.defaults)
- `CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024` — avoids captive portal 431 errors
- `CONFIG_DISPLAY_*` — display pins (matches StickS3)
- `CONFIG_BTN_*` — button pins + long-press threshold
- Custom partition table in `partitions.csv` (2MB factory)

Defaults in `sdkconfig.defaults` match StickS3 pinout.

## Project Structure

```
server/
  auth.py              CodeStore + DeviceStore + SessionStore
  server.py            FastAPI app, all endpoints + admin UI
firmware/
  main/
    main.cpp           App entry, boot flow, registration, main loop
    CMakeLists.txt     Font selection, component dependencies
    idf_component.yml  Managed components
    Kconfig            WiFi defaults
  components/
    auth_client/       HTTP client for server API
    auth_button/       Button input handling (short/long press)
    display/           ST7789 + LVGL display driver + UI
  partitions.csv       Custom partition table (2MB factory)
  sdkconfig.defaults   Default Kconfig values
web-flash/
  index.html           Web Serial flash page (esptool-js)
  authstick-merged.bin Merged firmware binary
```

## Key Design Decisions

- **HTTP in main task**: esp_http_client crashes when used from non-main FreeRTOS tasks. Registration runs in main task with `EspNetwork::CreateHttp()` (raw TCP via lwip, from 78/esp-ml307 component).
- **Display safety**: All LVGL operations run in main task only. WiFi callbacks only set flags.
- **Network timing**: 3-second delay after WiFi connect before HTTP. Network stack needs time to settle.
- **Font**: `font_puhui_basic_16_4` (16px 4bpp, Chinese) + `font_awesome_14_1` (14px 1bpp, icons). Matching xiaozhi-esp32 bread board config.
- **Display config**: 20MHz SPI, 52/40 gap, swap_xy=false, mirrored after voicestick StickS3 reference.

## License

MIT
