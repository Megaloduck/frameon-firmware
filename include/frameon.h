#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// frameon.h — Hardware, protocol, and buffer constants
//
// Hardware:  ESP32-S3-N16R8 (16 MB flash, 8 MB octal PSRAM)
// Panel:     P4-2121-64×32-32S-JHT3.0  HUB75E, 1/32 scan
// Library:   ESP32-HUB75-MatrixPanel-I2S-DMA v3.x
// ═══════════════════════════════════════════════════════════════════════════

// ─── GPIO ────────────────────────────────────────────────────────────────────
// All pins verified safe for ESP32-S3-N16R8
// (no strapping, no internal flash/PSRAM, no on-board LED conflicts)
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
#define PIN_E    42   // 5th address pin — required for 1/32 scan
#define PIN_CLK   2
#define PIN_LAT   1
#define PIN_OE   16

// ─── Panel geometry ──────────────────────────────────────────────────────────
// The physical panel is 64×32 with 1/32 scan (HUB75E, 5-bit row address).
// Configuring the library at PANEL_HEIGHT = 64 forces the E address bit to be
// driven — the standard workaround for 1/32-scan 64×32 panels.
// Only rows 0..(REAL_HEIGHT-1) contain actual physical pixels.
#define PANEL_WIDTH    64
#define PANEL_HEIGHT   64   // virtual — forces E-pin toggling
#define REAL_HEIGHT    32   // physical pixel rows on the panel
#define PANEL_CHAIN     1

// ─── Display ─────────────────────────────────────────────────────────────────
#define DEFAULT_BRIGHTNESS   128   // 0-255; 128 ≈ 50% for comfortable viewing

// ─── Protocol ────────────────────────────────────────────────────────────────
// Must exactly match Frameon's FrameExporter (frame_exporter.dart).
//
// Packet layout:
//   [0-2]   Magic: 0x46 0x52 0x4D ("FRM")
//   [3]     Version: 0x01
//   [4-5]   Frame count   (uint16 BE)
//   [6-7]   Width         (uint16 BE)  — expected 64
//   [8-9]   Height        (uint16 BE)  — expected 32
//   [10-11] Duration ms   (uint16 BE)  — uniform across all frames
//   [12-15] Payload bytes (uint32 BE)  — frame_count × width × height × 2
//   [16..]  RGB565 pixel data (big-endian, row-major)
//   [-2..]  CRC-16/CCITT (poly=0x1021, init=0xFFFF) over header + payload
//
#define FRM_MAGIC_0    0x46   // 'F'
#define FRM_MAGIC_1    0x52   // 'R'
#define FRM_MAGIC_2    0x4D   // 'M'
#define FRM_VERSION    0x01

#define HEADER_SIZE    16     // bytes before the pixel payload
#define CRC_SIZE        2     // bytes after the pixel payload

// ─── Serial responses (1 byte) ───────────────────────────────────────────────
#define RESP_ACK       0x06   // packet accepted, frames committed
#define RESP_NAK       0x15   // CRC mismatch — resend
#define RESP_ERR       0x1B   // malformed header / unsupported dimensions

// ─── Buffer limits ───────────────────────────────────────────────────────────
#define MAX_FRAMES     300                              // matches Flutter cap
#define FRAME_PIXELS   (PANEL_WIDTH * REAL_HEIGHT)     // 2048 pixels / frame
#define FRAME_BYTES    (FRAME_PIXELS * 2)              // 4096 bytes  / frame (RGB565)
#define MAX_PAYLOAD    ((uint32_t)MAX_FRAMES * FRAME_BYTES)    // ≈ 1.2 MB
#define MAX_PACKET     (HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE)  // ≈ 1.2 MB