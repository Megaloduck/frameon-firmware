// frameon.h — shared constants for the Frameon firmware protocol
//
// v1.5 — Clock overdraw support.
//        The app now sends a single-frame (or no-clock-frames) packet plus
//        a clock descriptor in the header. The firmware renders the clock
//        live on Core 0 using millis(), so time is always accurate with no
//        baked pixels and no loop-reset issues.
//
// Header layout (52 bytes, all multi-byte fields big-endian):
//
//   [0-2]   "FRM" magic
//   [3]     flags           0x02 = normal commit  |  0x4E = next-song preload
//   [4-5]   frameCount      uint16
//   [6-7]   width           uint16  (must equal PANEL_WIDTH)
//   [8-9]   height          uint16  (must equal REAL_HEIGHT)
//   [10-11] durMs           uint16  frame display duration in ms
//   [12-15] payloadBytes    uint32
//   [16-19] startPosMs      uint32  Spotify playback position at commit
//   [20-23] trackDurMs      uint32  Spotify track duration (0 = no bar)
//   [24]    barX            uint8   progress bar left edge
//   [25]    barY            uint8   progress bar top edge
//   [26]    barW            uint8   bar width (0 = no bar)
//   [27-28] barColor        uint16  bar foreground color RGB565
//   [29]    clockFlags      uint8   see CLK_FLAG_* below
//   [30-33] clockEpochSec   uint32  Unix time (seconds) at commit
//   [34-35] clockTzOffsetMin int16  signed timezone offset in minutes
//   [36]    clockFontId     uint8   0=Polymorph … 6=Phantasm
//   [37]    clockOffsetX    int8    horizontal pixel nudge (-128..127)
//   [38]    clockOffsetY    int8    vertical pixel nudge (-128..127)
//   [39]    reserved        uint8   0x00
//   [40-41] hoursColor      uint16  RGB565
//   [42-43] minutesColor    uint16  RGB565
//   [44-45] secondsColor    uint16  RGB565
//   [46-47] colonColor      uint16  RGB565
//   [48-49] dateColor       uint16  RGB565
//   [50-51] ampmColor       uint16  RGB565
//   [52..N] RGB565 pixel data
//   [N+1-N+2] CRC-16/CCITT
//
#pragma once

// ─── GPIO pin assignments ────────────────────────────────────────────────────
#define PIN_R1     4
#define PIN_G1     5
#define PIN_B1    12
#define PIN_R2    13
#define PIN_G2    14
#define PIN_B2    15
#define PIN_A     38
#define PIN_B     39
#define PIN_C     40
#define PIN_D     41
#define PIN_E     42
#define PIN_CLK    2
#define PIN_LAT    1
#define PIN_OE    16

// ─── Panel geometry ──────────────────────────────────────────────────────────
// PANEL_WIDTH / REAL_HEIGHT already defined below — only add the extras here.
#define PANEL_CHAIN         1
#define DEFAULT_BRIGHTNESS  128     
#define PANEL_WIDTH    64
#define PANEL_HEIGHT   64
#define REAL_HEIGHT    32       


#define FRM_MAGIC_0    0x46   // 'F'
#define FRM_MAGIC_1    0x52   // 'R'
#define FRM_MAGIC_2    0x4D   // 'M'
#define FRM_VERSION    0x02   // normal commit
#define FRM_NEXT       0x4E   // 'N' — queue as next-song preload

#define HEADER_SIZE    52     // v1.5: expanded for clock overdraw fields
#define CRC_SIZE        2

// ─── Clock flag bits (clockFlags byte, offset 29) ────────────────────────────
#define CLK_FLAG_PRESENT    0x01  // clock layer is active
#define CLK_FLAG_H12        0x02  // 12-hour format (else 24-hour)
#define CLK_FLAG_SECONDS    0x04  // show seconds
#define CLK_FLAG_DATE       0x08  // show date below time
#define CLK_FLAG_BLINK      0x10  // blink colon every 500 ms
#define CLK_FLAG_AMPM       0x20  // show AM/PM (only meaningful with CLK_FLAG_H12)

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