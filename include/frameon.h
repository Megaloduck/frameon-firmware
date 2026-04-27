#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// frameon.h — Hardware, protocol, and buffer constants
//
// Hardware:  ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM)
// Panel:     P4-2121-64×32-32S-JHT3.0  HUB75E, 1/32 scan
// Library:   ESP32-HUB75-MatrixPanel-I2S-DMA v3.x
// ═══════════════════════════════════════════════════════════════════════════

// ─── GPIO ────────────────────────────────────────────────────────────────────
#define PIN_R1    4
#define PIN_G1    5
#define PIN_B1   12
#define PIN_R2   13
#define PIN_G2   14
#define PIN_B2   15
#define PIN_A    38
#define PIN_B    39
#define PIN_C    40
#define PIN_D    41
#define PIN_E    42
#define PIN_CLK   2
#define PIN_LAT   1
#define PIN_OE   16

// ─── Panel geometry ──────────────────────────────────────────────────────────
#define PANEL_WIDTH    64
#define PANEL_HEIGHT   64
#define REAL_HEIGHT    32
#define PANEL_CHAIN     1

// ─── Display ─────────────────────────────────────────────────────────────────
#define DEFAULT_BRIGHTNESS   128

// ─── Protocol v1.4 ───────────────────────────────────────────────────────────
//
// Header layout (30 bytes, all multi-byte fields big-endian):
//
//   [0-2]   Magic:           0x46 0x52 0x4D  ("FRM")
//   [3]     Flags:           0x02 = normal commit  |  0x4E = next-song preload
//   [4-5]   Frame count      (uint16 BE)
//   [6-7]   Width            (uint16 BE)  — expected 64
//   [8-9]   Height           (uint16 BE)  — expected 32
//   [10-11] Duration ms      (uint16 BE)  — per-frame duration
//   [12-15] Payload bytes    (uint32 BE)
//   [16-19] startPositionMs  (uint32 BE)  — song position at packet build time
//   [20-23] trackDurationMs  (uint32 BE)  — total song length (0 = no overdraw)
//   [24]    barX             (uint8)      — progress bar left edge (px)
//   [25]    barY             (uint8)      — progress bar top edge (px)
//   [26]    barW             (uint8)      — progress bar width (px, 0 = no bar)
//   [27-28] barColor         (uint16 BE)  — filled portion color in RGB565
//   [29]    _reserved        (uint8)      — pad to 30 bytes, must be 0x00
//   [30..]  RGB565 pixel data
//   [-2..]  CRC-16/CCITT over header + payload
//
// v1.4 adds barColor (uint16 BE RGB565) so the firmware uses the exact color
// chosen in the Dart layer settings instead of the hardcoded Spotify green.
// barColor is computed by the app from layer.progressColor at export time.
//
#define FRM_MAGIC_0    0x46   // 'F'
#define FRM_MAGIC_1    0x52   // 'R'
#define FRM_MAGIC_2    0x4D   // 'M'
#define FRM_VERSION    0x02   // normal commit
#define FRM_NEXT       0x4E   // 'N' — queue as next-song preload

#define HEADER_SIZE    30     // v1.4: was 28, +2 for barColor(uint16) replacing reserved
#define CRC_SIZE        2

// ─── Serial responses ────────────────────────────────────────────────────────
#define RESP_ACK       0x06
#define RESP_NAK       0x15
#define RESP_ERR       0x1B

// ─── Buffer limits ───────────────────────────────────────────────────────────
#define MAX_FRAMES     300
#define FRAME_PIXELS   (PANEL_WIDTH * REAL_HEIGHT)
#define FRAME_BYTES    (FRAME_PIXELS * 2)
#define MAX_PAYLOAD    ((uint32_t)MAX_FRAMES * FRAME_BYTES)
#define MAX_PACKET     (HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE)