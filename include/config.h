#pragma once

// ── Panel geometry ────────────────────────────────────────────────────────
#define PANEL_WIDTH      64
#define PANEL_HEIGHT     32
#define PANEL_CHAIN      1      // number of panels chained horizontally
#define PANEL_BRIGHTNESS 128    // 0–255, default startup brightness

// ── HUB75 pin mapping (ESP32-S3 custom wiring) ───────────────────────────
#define PIN_R1   4
#define PIN_G1   5
#define PIN_B1  12
#define PIN_R2  13
#define PIN_G2  14
#define PIN_B2  15
#define PIN_A   38
#define PIN_B   39
#define PIN_C   40
#define PIN_D   41
#define PIN_E   42   // 1/32 scan — required for 32-row panels
#define PIN_CLK  2
#define PIN_LAT  1
#define PIN_OE  16

// ── Serial provisioning ───────────────────────────────────────────────────
#define SERIAL_BAUD      115200
#define SERIAL_TIMEOUT   200    // ms between JSON parse attempts

// ── Wi-Fi ─────────────────────────────────────────────────────────────────
#define WIFI_CONNECT_TIMEOUT  15000  // ms before giving up
#define WIFI_RETRY_INTERVAL   30000  // ms between reconnect attempts

// ── API ───────────────────────────────────────────────────────────────────
#define API_PORT          80
#define WS_PATH           "/ws"
#define OTA_PATH          "/update"

// ── NTP ───────────────────────────────────────────────────────────────────
#define NTP_DEFAULT_SERVER  "pool.ntp.org"
#define NTP_UPDATE_INTERVAL  60000   // ms between NTP syncs

// ── App modes ─────────────────────────────────────────────────────────────
enum AppMode {
  MODE_CLOCK   = 0,
  MODE_SPOTIFY = 1,
  MODE_GIF     = 2,
  MODE_POMODORO = 3
};

// ── Preferences keys ──────────────────────────────────────────────────────
#define PREF_NS            "frameon"
#define PREF_SSID          "ssid"
#define PREF_PASS          "pass"
#define PREF_MODE          "mode"
#define PREF_BRIGHTNESS    "bright"
#define PREF_CLOCK_24H     "c24h"
#define PREF_CLOCK_DATE    "cdate"
#define PREF_CLOCK_SECS    "csecs"
#define PREF_TIMEZONE      "tz"
#define PREF_NTP_SERVER    "ntp"
#define PREF_POMO_WORK     "pwork"
#define PREF_POMO_SHORT    "pshort"
#define PREF_POMO_LONG     "plong"
#define PREF_POMO_SESSIONS "psess"
