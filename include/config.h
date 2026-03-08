#pragma once

// ═══════════════════════════════════════════════════════════════════════════
//  config.h  —  Frameon ESP32-S3 firmware
//
//  Edit this file to match your hardware.  Everything else reads from here.
// ═══════════════════════════════════════════════════════════════════════════

// ── Panel geometry ────────────────────────────────────────────────────────────
// Must match the -DPANEL_WIDTH / -DPANEL_HEIGHT defines in platformio.ini
#ifndef PANEL_WIDTH
#  define PANEL_WIDTH   64
#endif
#ifndef PANEL_HEIGHT
#  define PANEL_HEIGHT  32
#endif
#ifndef CHAIN_LENGTH
#  define CHAIN_LENGTH  1
#endif

static constexpr uint16_t MATRIX_COLS   = PANEL_WIDTH  * CHAIN_LENGTH;
static constexpr uint16_t MATRIX_ROWS   = PANEL_HEIGHT;
static constexpr uint32_t FRAME_BYTES   = MATRIX_COLS * MATRIX_ROWS * 2; // RGB565

// ── HUB75 pin mapping (ESP32-S3 DevKit C-1) ──────────────────────────────────
//
//  Wiring guide (one common layout — adjust to your board):
//
//   HUB75 pin  │ ESP32-S3 GPIO
//  ────────────┼──────────────
//   R1         │  GPIO 25
//   G1         │  GPIO 26
//   B1         │  GPIO 27
//   R2         │  GPIO 14
//   G2         │  GPIO 12
//   B2         │  GPIO 13
//   A  (row)   │  GPIO 23
//   B  (row)   │  GPIO 19
//   C  (row)   │  GPIO 5
//   D  (row)   │  GPIO 17
//   E  (row)   │  GPIO 18   ← only needed for 64-row panels; set -1 for 32-row
//   CLK        │  GPIO 16
//   LAT        │  GPIO 4
//   OE         │  GPIO 15
//
//  These are passed to MatrixPanel_I2S_DMA / HUB75_I2S_CFG.

#define HUB75_PIN_R1   25
#define HUB75_PIN_G1   26
#define HUB75_PIN_B1   27
#define HUB75_PIN_R2   14
#define HUB75_PIN_G2   12
#define HUB75_PIN_B2   13
#define HUB75_PIN_A    23
#define HUB75_PIN_B    19
#define HUB75_PIN_C    5
#define HUB75_PIN_D    17
#define HUB75_PIN_E    -1   // -1 for 32-row panels
#define HUB75_PIN_CLK  16
#define HUB75_PIN_LAT  4
#define HUB75_PIN_OE   15

// ── BLE ───────────────────────────────────────────────────────────────────────
// These UUIDs MUST match lib/core/ble/ble_uuids.dart exactly.

#define BLE_DEVICE_NAME         "Frameon"
#define BLE_SERVICE_UUID        "12345678-1234-1234-1234-123456789abc"
#define BLE_CHAR_FRAME_DATA     "12345678-1234-1234-1234-123456789ab1"
#define BLE_CHAR_CONTROL        "12345678-1234-1234-1234-123456789ab2"
#define BLE_CHAR_STATUS         "12345678-1234-1234-1234-123456789ab3"
#define BLE_CHAR_CLOCK_CONFIG   "12345678-1234-1234-1234-123456789ab4"
#define BLE_CHAR_GIF_META       "12345678-1234-1234-1234-123456789ab5"

// ── Control command bytes (mirror ble_uuids.dart) ─────────────────────────────
#define CMD_FRAME_BEGIN   0x01
#define CMD_FRAME_COMMIT  0x02
#define CMD_CLEAR         0x03
#define CMD_SET_MODE      0x04
#define CMD_SET_BRIGHT    0x05
#define CMD_ABORT         0x06
#define CMD_PING          0x07

// ── Status bytes sent back to phone via notify ────────────────────────────────
#define STATUS_OK         0x00
#define STATUS_ERROR      0x01
#define STATUS_BUSY       0x02
#define STATUS_READY      0x03

// ── Display modes (mirror ble_uuids.dart) ─────────────────────────────────────
#define MODE_STILL        0x00
#define MODE_GIF          0x01
#define MODE_CLOCK        0x02
#define MODE_SPOTIFY      0x03
#define MODE_PIXEL_ART    0x04

// ── Transfer buffer ───────────────────────────────────────────────────────────
// Must hold one full frame.  4 096 bytes for 64×32 RGB565.
// We allocate this in PSRAM if available so it doesn't eat SRAM.
#define FRAME_BUF_SIZE    FRAME_BYTES       // 4 096 bytes

// Max GIF frames we'll store in RAM/PSRAM.
// At 4 096 bytes/frame × 64 = 256 KB — fits in 4 MB PSRAM with room to spare.
#define GIF_MAX_FRAMES    64
#define GIF_MAX_DURATION  65535  // ms, per-frame cap (u16 in wire protocol)

// ── Brightness ────────────────────────────────────────────────────────────────
#define DEFAULT_BRIGHTNESS  128   // 0–255, maps to MatrixPanel brightness 0–100

// ── Clock ─────────────────────────────────────────────────────────────────────
// How often the clock face redraws (ms)
#define CLOCK_REFRESH_MS    500

// ── GIF playback ─────────────────────────────────────────────────────────────
// Minimum frame duration enforced on playback regardless of metadata
#define GIF_MIN_FRAME_MS    16

// ── Task priorities ───────────────────────────────────────────────────────────
#define TASK_PRIO_BLE       2
#define TASK_PRIO_DISPLAY   3   // higher = runs first when both are ready
#define TASK_PRIO_CLOCK     1

// ── Debug ─────────────────────────────────────────────────────────────────────
#define FRAMEON_DEBUG  1   // set to 0 for production builds
#if FRAMEON_DEBUG
#  define LOG(fmt, ...) Serial.printf("[Frameon] " fmt "\n", ##__VA_ARGS__)
#else
#  define LOG(fmt, ...) do {} while(0)
#endif