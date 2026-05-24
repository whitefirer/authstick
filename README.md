# xiaozhi-stick-auth

StickS3 firmware for terminal login authentication. Bypasses LLM TTS by rendering
verification codes directly on the StickS3 display. Physical buttons for approve/deny.

## Why

xiaozhi-esp32 is great for voice interaction, but terminal auth codes leak through TTS.
This firmware handles auth display natively:

- Codes rendered on screen, never spoken
- Physical buttons for instant approve/deny
- Device MAC provides natural multi-terminal isolation
- WiFi direct to Auth Server — no desktop app needed

## Hardware

- M5Stack StickS3 (ESP32-S3-PICO-1-N8R8)
- 135×240 ST7789 display
- ES8311 audio codec (optional)
- 2 physical buttons (front + side)

## Architecture

```
StickS3 ──WiFi──→ Auth Server (:9998)
  │                  │
  ├─ GET /api/verify/pending?device=MAC    ← poll for auth requests
  ├─ POST /api/verify/approve {code}       ← button A press
  ├─ POST /api/verify/deny {code}          ← button B press
  └─ Display shows code + countdown

Auth Server new endpoints:
  GET  /api/verify/pending?device=MAC → {code, service, expires}
  POST /api/verify/approve            → {ok: true}
  POST /api/verify/deny               → {ok: true}
```

## Flashing

Web-based flashing tool (from voicestick):
1. Open https://your-server/web-flash
2. Connect StickS3 via USB
3. Click flash — firmware written directly from browser

## Project Structure

```
firmware/
  main/              ESP-IDF app entry
  components/
    auth_client/     HTTP client for Auth Server API
    display/         ST7789 driver + UI rendering
    button/          Button input handling
web-flash/           Web-based firmware flashing tool
```

## License

MIT
