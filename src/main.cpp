/*
 * Frameon Firmware v1.6
 * ESP32-S3  ·  P4-2121-64×32 HUB75E
 *
 * v1.6 — Full multi-font clock support
 *   All 7 Frameon fonts (Polymorph, Brickwork, Waterfox, Vandalism,
 *   Destroked, Stereotype, Phantasm) are now embedded in the firmware.
 *   overdrawClock() selects the active font via the fontId byte in the
 *   packet header, exactly matching the Dart-side font selection.
 *
 * v1.5 — Clock overdraw
 *   The app no longer bakes clock digits into frame pixels. Instead the header
 *   carries a Unix timestamp (clockEpochSec) recorded at commit time plus
 *   clock display flags. displayTask derives the current wall-clock time from:
 *
 *       wallSec = clockEpochSec + (millis() - commitTimeMs) / 1000
 *
 *   and overdraw the clock on top of the rendered frame, exactly like
 *   overdrawProgressBar does for Spotify. This means:
 *     • Seconds always tick correctly, with no loop-reset issues.
 *     • Minutes and hours update naturally forever after one sync.
 *     • Blink-colon uses millis() % 1000 < 500 — always accurate.
 *     • No latency compensation needed — commitTimeMs is set at ACK time.
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"
#include "waitingscreen.h"

MatrixPanel_I2S_DMA* matrix = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Double-buffer PSRAM storage
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t* pktBuf[2] = {nullptr, nullptr};
static int      pendingBuf = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Active display state (written under swapMutex by Core 1, read by Core 0)
// ─────────────────────────────────────────────────────────────────────────────

static volatile int      activeBuf           = 0;
static volatile int      activeFrameCount    = 0;
static volatile uint16_t activeFrameDurMs    = 100;

// Spotify progress bar
static volatile uint32_t activeStartPosMs    = 0;
static volatile uint32_t activeTrackDurMs    = 0;
static volatile uint32_t commitTimeMs        = 0;
static volatile uint8_t  activeBarX          = 0;
static volatile uint8_t  activeBarY          = 0;
static volatile uint8_t  activeBarW          = 0;
static volatile uint16_t activeBarColor      = 0x11C5;

// Clock overdraw state (v1.5)
static volatile uint8_t  activeClockFlags    = 0;
static volatile uint32_t activeClockEpochSec = 0;
static volatile int16_t  activeClockTzMin    = 0;
static volatile uint8_t  activeClockFontId   = 0;
static volatile int8_t   activeClockOffsetX  = 0;
static volatile int8_t   activeClockOffsetY  = 0;
static volatile uint16_t activeHoursColor    = 0x07E0;
static volatile uint16_t activeMinutesColor  = 0x07E0;
static volatile uint16_t activeSecondsColor  = 0x07E0;
static volatile uint16_t activeColonColor    = 0x07E0;
static volatile uint16_t activeDateColor     = 0x07E0;
static volatile uint16_t activeAmpmColor     = 0x07E0;

// Next-song preload
static uint8_t*          nextBuf      = nullptr;
static volatile bool     nextBufReady = false;
static volatile int      nextFrameCount = 0;
static volatile uint16_t nextFrameDurMs = 100;
static volatile uint32_t nextStartPosMs = 0;
static volatile uint32_t nextTrackDurMs = 0;
static volatile uint8_t  nextBarX = 0;
static volatile uint8_t  nextBarY = 0;
static volatile uint8_t  nextBarW = 0;
static volatile uint16_t nextBarColor = 0x11C5;

static SemaphoreHandle_t swapMutex = nullptr;
static SemaphoreHandle_t nextMutex = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Serial receive state machine
// ─────────────────────────────────────────────────────────────────────────────

enum RxState : uint8_t { RX_IDLE, RX_HEADER, RX_PAYLOAD, RX_CRC_H, RX_CRC_L };

static RxState  rxState    = RX_IDLE;
static uint32_t rxIdx      = 0;
static uint8_t  syncBuf[3] = {0, 0, 0};

static uint8_t  pktFlags        = FRM_VERSION;
static uint16_t pktFrameCount   = 0;
static uint16_t pktWidth        = 0;
static uint16_t pktHeight       = 0;
static uint16_t pktDurMs        = 0;
static uint32_t pktPayloadBytes = 0;
static uint32_t pktStartPosMs   = 0;
static uint32_t pktTrackDurMs   = 0;
static uint8_t  pktBarX         = 0;
static uint8_t  pktBarY         = 0;
static uint8_t  pktBarW         = 0;
static uint16_t pktBarColor     = 0x11C5;
// v1.5 clock fields
static uint8_t  pktClockFlags    = 0;
static uint32_t pktClockEpochSec = 0;
static int16_t  pktClockTzMin    = 0;
static uint8_t  pktClockFontId   = 0;
static int8_t   pktClockOffsetX  = 0;
static int8_t   pktClockOffsetY  = 0;
static uint16_t pktHoursColor    = 0x07E0;
static uint16_t pktMinutesColor  = 0x07E0;
static uint16_t pktSecondsColor  = 0x07E0;
static uint16_t pktColonColor    = 0x07E0;
static uint16_t pktDateColor     = 0x07E0;
static uint16_t pktAmpmColor     = 0x07E0;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len);
static void     renderFrame(int bufIdx, int frameIdx);
static void     overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs,
                                    uint8_t barX, uint8_t barY, uint8_t barW,
                                    uint16_t barColor);
static void     overdrawClock(uint32_t epochSec, uint32_t elapsedMs,
                               int16_t tzOffsetMin, uint8_t flags,
                               uint8_t fontId, int8_t offX, int8_t offY,
                               uint16_t hoursCol, uint16_t minutesCol,
                               uint16_t secondsCol, uint16_t colonCol,
                               uint16_t dateCol,   uint16_t ampmCol);
static void     parseHeader();
static void     processPacket();
static void     processSerial();
static void     displayTask(void* param);

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16/CCITT — poly 0x1021, init 0xFFFF
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// renderFrame — blit one frame from the packet buffer to the matrix
// ─────────────────────────────────────────────────────────────────────────────

static void renderFrame(int bufIdx, int frameIdx) {
    const uint8_t* src = pktBuf[bufIdx]
                       + HEADER_SIZE
                       + (uint32_t)frameIdx * FRAME_BYTES;
    for (int y = 0; y < REAL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            const int      i     = y * PANEL_WIDTH + x;
            const uint16_t color = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
            matrix->drawPixel(x, y, color);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawProgressBar
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs,
                                 uint8_t barX, uint8_t barY, uint8_t barW,
                                 uint16_t barColor) {
    if (trackDurMs == 0 || barW == 0) return;
    float p = (float)songPosMs / (float)trackDurMs;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const int filled = (int)(barW * p + 0.5f);
    const uint16_t bgColor = 0x3186;
    for (int row = barY; row <= barY + 1; row++) {
        for (int x = 0; x < barW; x++) {
            matrix->drawPixel(barX + x, row, x < filled ? barColor : bgColor);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-font clock renderer (v1.6)
//
// Each font table has 16 entries, indexed as:
//   0-9  → '0'-'9'
//   10   → ':'
//   11   → '.'
//   12   → 'A'
//   13   → 'M'
//   14   → 'P'
//   15   → ' ' (space / fallback)
//
// Glyph format: w = pixel width, rows[7] = 7-row bitmask, MSB = leftmost pixel.
// Sourced directly from lib/engine/renderer/fonts/*.dart
// ─────────────────────────────────────────────────────────────────────────────

struct Glyph { uint8_t w; uint8_t rows[7]; };

static int glyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '.') return 11;
    if (c == 'A') return 12;
    if (c == 'M') return 13;
    if (c == 'P') return 14;
    return 15;
}

// ── 0: Polymorph ─────────────────────────────────────────────────────────────
static const Glyph kFontPolymorph[16] = {
    {4, {0x06,0x09,0x09,0x09,0x09,0x09,0x06}}, // 0
    {2, {0x03,0x01,0x01,0x01,0x01,0x01,0x01}}, // 1
    {4, {0x06,0x09,0x01,0x02,0x04,0x08,0x0F}}, // 2
    {4, {0x06,0x09,0x01,0x06,0x01,0x09,0x06}}, // 3
    {4, {0x01,0x03,0x05,0x09,0x0F,0x01,0x01}}, // 4
    {4, {0x0F,0x08,0x0E,0x01,0x01,0x09,0x06}}, // 5
    {4, {0x06,0x09,0x08,0x0E,0x09,0x09,0x06}}, // 6
    {4, {0x0F,0x01,0x02,0x02,0x04,0x04,0x04}}, // 7
    {4, {0x06,0x09,0x09,0x06,0x09,0x09,0x06}}, // 8
    {4, {0x06,0x09,0x09,0x07,0x01,0x09,0x06}}, // 9
    {1, {0x00,0x01,0x00,0x00,0x00,0x01,0x00}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}}, // .
    {5, {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}}, // A
    {5, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}}, // M
    {5, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, // P
    {3, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 1: Brickwork ─────────────────────────────────────────────────────────────
static const Glyph kFontBrickwork[16] = {
    {5, {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, // 0
    {5, {0x04,0x0C,0x04,0x04,0x04,0x04,0x1F}}, // 1
    {5, {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}, // 2
    {5, {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}}, // 3
    {5, {0x03,0x05,0x09,0x11,0x1F,0x01,0x01}}, // 4
    {5, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}, // 5
    {5, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, // 6
    {5, {0x1F,0x11,0x01,0x02,0x04,0x04,0x04}}, // 7
    {5, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, // 8
    {5, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}, // 9
    {1, {0x00,0x01,0x01,0x00,0x00,0x01,0x01}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x01,0x01}}, // .
    {5, {0x0E,0x11,0x1F,0x11,0x11,0x11,0x11}}, // A
    {5, {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}}, // M
    {5, {0x1E,0x11,0x1E,0x10,0x10,0x10,0x10}}, // P
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 2: Waterfox ──────────────────────────────────────────────────────────────
// Note: '0' and '8' are 6px wide; 'A' is 7px; 'M' is 9px (clamped to 8).
static const Glyph kFontWaterfox[16] = {
    {6, {0x0C,0x12,0x21,0x21,0x21,0x12,0x0C}}, // 0  (w=6)
    {3, {0x02,0x06,0x02,0x02,0x02,0x02,0x07}}, // 1
    {5, {0x0E,0x11,0x01,0x02,0x04,0x09,0x1F}}, // 2
    {5, {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}}, // 3
    {5, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x07}}, // 4
    {5, {0x1F,0x10,0x16,0x19,0x01,0x11,0x0E}}, // 5
    {5, {0x0E,0x10,0x16,0x19,0x11,0x11,0x0E}}, // 6
    {5, {0x1F,0x11,0x02,0x02,0x04,0x04,0x04}}, // 7
    {6, {0x0C,0x12,0x12,0x1E,0x21,0x21,0x1E}}, // 8  (w=6)
    {5, {0x0E,0x11,0x11,0x13,0x0D,0x01,0x0E}}, // 9
    {1, {0x00,0x00,0x01,0x00,0x00,0x00,0x01}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}}, // .
    {7, {0x08,0x14,0x14,0x14,0x3E,0x22,0x77}}, // A  (w=7)
    {8, {0x83,0xC6,0xC6,0xAA,0xAA,0x92,0xD7}}, // M  (w=9 → low 8 bits)
    {6, {0x3E,0x11,0x11,0x1E,0x10,0x10,0x38}}, // P  (w=6)
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 3: Vandalism ─────────────────────────────────────────────────────────────
static const Glyph kFontVandalism[16] = {
    {5, {0x0E,0x19,0x1B,0x1D,0x1D,0x19,0x0E}}, // 0
    {3, {0x02,0x06,0x06,0x02,0x02,0x02,0x07}}, // 1
    {5, {0x0E,0x19,0x19,0x02,0x04,0x0C,0x1F}}, // 2
    {5, {0x0E,0x19,0x01,0x06,0x01,0x19,0x0E}}, // 3
    {5, {0x06,0x0E,0x0A,0x1A,0x1F,0x02,0x07}}, // 4
    {5, {0x0F,0x18,0x18,0x1E,0x01,0x19,0x0E}}, // 5
    {5, {0x0E,0x19,0x18,0x1E,0x19,0x19,0x0E}}, // 6
    {5, {0x0E,0x19,0x19,0x02,0x04,0x04,0x04}}, // 7
    {5, {0x0E,0x19,0x19,0x0E,0x19,0x19,0x0E}}, // 8
    {5, {0x0E,0x19,0x19,0x0F,0x01,0x11,0x0E}}, // 9
    {1, {0x00,0x00,0x01,0x00,0x00,0x01,0x00}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}}, // .
    {5, {0x06,0x0E,0x0E,0x1F,0x19,0x19,0x19}}, // A
    {5, {0x19,0x1B,0x1F,0x1D,0x1D,0x19,0x19}}, // M
    {5, {0x1E,0x0D,0x0D,0x0E,0x0C,0x0C,0x1E}}, // P
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 4: Destroked ─────────────────────────────────────────────────────────────
static const Glyph kFontDestroked[16] = {
    {5, {0x0E,0x1B,0x11,0x15,0x11,0x1B,0x0E}}, // 0
    {3, {0x02,0x04,0x02,0x02,0x02,0x02,0x02}}, // 1
    {5, {0x16,0x09,0x01,0x0E,0x10,0x12,0x1D}}, // 2
    {5, {0x0F,0x12,0x02,0x04,0x02,0x11,0x0E}}, // 3
    {5, {0x19,0x12,0x11,0x1D,0x03,0x01,0x01}}, // 4
    {5, {0x1F,0x08,0x10,0x1F,0x01,0x0A,0x14}}, // 5
    {5, {0x0E,0x1B,0x10,0x1E,0x11,0x1B,0x0E}}, // 6
    {5, {0x0F,0x12,0x04,0x08,0x04,0x04,0x04}}, // 7
    {5, {0x0F,0x12,0x11,0x0E,0x11,0x09,0x16}}, // 8
    {5, {0x0D,0x12,0x11,0x0F,0x01,0x09,0x16}}, // 9
    {1, {0x00,0x02,0x00,0x00,0x00,0x02,0x00}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}}, // .
    {5, {0x16,0x09,0x09,0x0F,0x09,0x09,0x09}}, // A
    {7, {0x61,0x5A,0x52,0x4A,0x4A,0x4A,0x49}}, // M  (w=7)
    {5, {0x1E,0x09,0x09,0x0E,0x08,0x08,0x08}}, // P
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 5: Stereotype ────────────────────────────────────────────────────────────
static const Glyph kFontStereotype[16] = {
    {4, {0x06,0x09,0x0B,0x0D,0x09,0x09,0x06}}, // 0
    {3, {0x01,0x03,0x05,0x01,0x01,0x01,0x01}}, // 1
    {4, {0x06,0x09,0x01,0x02,0x04,0x08,0x0F}}, // 2
    {4, {0x06,0x09,0x01,0x07,0x01,0x09,0x06}}, // 3
    {4, {0x01,0x03,0x05,0x09,0x0F,0x01,0x01}}, // 4
    {4, {0x0F,0x08,0x08,0x0E,0x01,0x09,0x06}}, // 5
    {4, {0x06,0x09,0x08,0x0E,0x09,0x09,0x06}}, // 6
    {4, {0x0F,0x01,0x02,0x04,0x08,0x08,0x08}}, // 7
    {4, {0x06,0x09,0x09,0x06,0x09,0x09,0x06}}, // 8
    {4, {0x06,0x09,0x09,0x07,0x01,0x09,0x06}}, // 9
    {2, {0x00,0x03,0x00,0x00,0x00,0x00,0x03}}, // :
    {2, {0x00,0x00,0x00,0x00,0x02,0x02,0x04}}, // .
    {4, {0x06,0x09,0x09,0x09,0x0F,0x09,0x09}}, // A
    {7, {0x41,0x63,0x55,0x49,0x41,0x41,0x41}}, // M  (w=7)
    {4, {0x0E,0x09,0x09,0x0E,0x08,0x08,0x08}}, // P
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// ── 6: Phantasm ──────────────────────────────────────────────────────────────
static const Glyph kFontPhantasm[16] = {
    {5, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, // 0
    {3, {0x02,0x06,0x02,0x02,0x02,0x02,0x07}}, // 1
    {5, {0x0E,0x11,0x01,0x06,0x08,0x10,0x1F}}, // 2
    {5, {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}}, // 3
    {5, {0x02,0x12,0x12,0x12,0x1F,0x02,0x02}}, // 4
    {5, {0x1F,0x10,0x10,0x1F,0x01,0x11,0x0E}}, // 5
    {5, {0x0E,0x11,0x10,0x1E,0x11,0x11,0x0E}}, // 6
    {5, {0x1F,0x01,0x01,0x02,0x04,0x04,0x04}}, // 7
    {5, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, // 8
    {5, {0x0E,0x11,0x11,0x0F,0x01,0x11,0x0E}}, // 9
    {1, {0x00,0x00,0x01,0x00,0x00,0x01,0x00}}, // :
    {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}}, // .
    {5, {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}}, // A
    {7, {0x41,0x63,0x55,0x49,0x41,0x41,0x41}}, // M  (w=7)
    {5, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}}, // P
    {3, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' '
};

// Font table — indexed by fontId (0=Polymorph … 6=Phantasm)
static const Glyph* const kFonts[7] = {
    kFontPolymorph,
    kFontBrickwork,
    kFontWaterfox,
    kFontVandalism,
    kFontDestroked,
    kFontStereotype,
    kFontPhantasm,
};

// Active font pointer — set at the start of overdrawClock()
static const Glyph* gActiveFont = kFontPolymorph;

static int glyphWidth(char c) { return gActiveFont[glyphIndex(c)].w + 1; } // +1 gap

static int textWidth(const char* s) {
    int w = 0;
    while (*s) { w += glyphWidth(*s++); }
    return w > 0 ? w - 1 : 0; // no trailing gap
}

static void drawGlyph(char c, int x, int y, uint16_t color) {
    const Glyph& g = gActiveFont[glyphIndex(c)];
    for (int row = 0; row < 7; row++) {
        const uint8_t bits = g.rows[row];
        for (int col = 0; col < g.w; col++) {
            if ((bits >> (g.w - 1 - col)) & 1) {
                matrix->drawPixel(x + col, y + row, color);
            }
        }
    }
}

static void drawText(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        drawGlyph(*s, x, y, color);
        x += glyphWidth(*s);
        s++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Simple time struct
// ─────────────────────────────────────────────────────────────────────────────

struct ClockTime { int hour, minute, second, day, month, year; };

static const uint8_t kDaysInMonth[2][12] = {
    {31,28,31,30,31,30,31,31,30,31,30,31},
    {31,29,31,30,31,30,31,31,30,31,30,31},
};

static bool isLeap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static ClockTime epochToTime(uint32_t epochSec, int16_t tzOffsetMin) {
    int32_t t = (int32_t)epochSec + (int32_t)tzOffsetMin * 60;
    if (t < 0) t = 0;

    ClockTime ct;
    ct.second = t % 60; t /= 60;
    ct.minute = t % 60; t /= 60;
    ct.hour   = t % 24; t /= 24;

    uint32_t days = (uint32_t)t;
    int y = 1970;
    while (true) {
        uint32_t diy = isLeap(y) ? 366 : 365;
        if (days < diy) break;
        days -= diy;
        y++;
    }
    ct.year = y;
    int leap = isLeap(y) ? 1 : 0;
    int m = 1;
    while (m <= 12 && days >= kDaysInMonth[leap][m-1]) {
        days -= kDaysInMonth[leap][m-1];
        m++;
    }
    ct.month = m;
    ct.day   = (int)days + 1;
    return ct;
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawClock — renders clock on top of the current frame
// fontId: 0=Polymorph 1=Brickwork 2=Waterfox 3=Vandalism
//          4=Destroked 5=Stereotype 6=Phantasm
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawClock(uint32_t epochSec, uint32_t elapsedMs,
                           int16_t tzOffsetMin, uint8_t flags,
                           uint8_t fontId, int8_t offX, int8_t offY,
                           uint16_t hoursCol,   uint16_t minutesCol,
                           uint16_t secondsCol, uint16_t colonCol,
                           uint16_t dateCol,    uint16_t ampmCol) {

    if (!(flags & CLK_FLAG_PRESENT)) return;

    // Select font — clamp to valid range
    gActiveFont = kFonts[fontId < 7 ? fontId : 0];

    const bool h12      = flags & CLK_FLAG_H12;
    const bool showSec  = flags & CLK_FLAG_SECONDS;
    const bool showDate = flags & CLK_FLAG_DATE;
    const bool blinkCol = flags & CLK_FLAG_BLINK;
    const bool showAmPm = flags & CLK_FLAG_AMPM;

    const uint32_t wallSec    = epochSec + elapsedMs / 1000;
    ClockTime      ct         = epochToTime(wallSec, tzOffsetMin);
    const bool     colonVisible = !blinkCol || (elapsedMs % 1000) < 500;

    char hBuf[4], mBuf[4], sBuf[4], ampmBuf[4], dateBuf[10];

    int dispHour = ct.hour;
    if (h12) {
        dispHour = ct.hour % 12;
        if (dispHour == 0) dispHour = 12;
        snprintf(hBuf,    sizeof(hBuf),    "%d",  dispHour);
        snprintf(ampmBuf, sizeof(ampmBuf), "%s",  ct.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hBuf,    sizeof(hBuf),    "%02d", dispHour);
        ampmBuf[0] = '\0';
    }
    snprintf(mBuf,    sizeof(mBuf),    "%02d", ct.minute);
    snprintf(sBuf,    sizeof(sBuf),    "%02d", ct.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%02d",
             ct.day, ct.month, ct.year % 100);

    int timeW = textWidth(hBuf)
              + 2 + glyphWidth(':')
              + textWidth(mBuf);
    if (showSec)   timeW += 2 + glyphWidth(':') + textWidth(sBuf);
    if (showAmPm)  timeW += 1 + textWidth(ampmBuf);

    const int charH  = 7;
    const int totalH = charH + (showDate ? charH + 2 : 0);
    const int startY = (REAL_HEIGHT - totalH) / 2 + (int)offY;
    const int timeY  = startY;
    const int dateY  = startY + charH + 2;

    int cx = (PANEL_WIDTH - timeW) / 2 + (int)offX;

    drawText(hBuf, cx, timeY, hoursCol);
    cx += textWidth(hBuf);

    cx += 2;
    if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
    cx += glyphWidth(':') - 1;

    drawText(mBuf, cx, timeY, minutesCol);
    cx += textWidth(mBuf);

    if (showSec) {
        cx += 2;
        if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
        cx += glyphWidth(':') - 1;
        drawText(sBuf, cx, timeY, secondsCol);
        cx += textWidth(sBuf);
    }

    if (showAmPm) {
        cx += 1;
        drawText(ampmBuf, cx, timeY, ampmCol);
    }

    if (showDate) {
        int dw = textWidth(dateBuf);
        int dx = (PANEL_WIDTH - dw) / 2 + (int)offX;
        drawText(dateBuf, dx, dateY, dateCol);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHeader — v1.5
// ─────────────────────────────────────────────────────────────────────────────

static void parseHeader() {
    const uint8_t* h = pktBuf[pendingBuf];
    pktFlags         =  h[3];
    pktFrameCount    = ((uint16_t)h[4]  << 8) | h[5];
    pktWidth         = ((uint16_t)h[6]  << 8) | h[7];
    pktHeight        = ((uint16_t)h[8]  << 8) | h[9];
    pktDurMs         = ((uint16_t)h[10] << 8) | h[11];
    pktPayloadBytes  = ((uint32_t)h[12] << 24)
                     | ((uint32_t)h[13] << 16)
                     | ((uint32_t)h[14] <<  8)
                     |            h[15];
    pktStartPosMs    = ((uint32_t)h[16] << 24)
                     | ((uint32_t)h[17] << 16)
                     | ((uint32_t)h[18] <<  8)
                     |            h[19];
    pktTrackDurMs    = ((uint32_t)h[20] << 24)
                     | ((uint32_t)h[21] << 16)
                     | ((uint32_t)h[22] <<  8)
                     |            h[23];
    pktBarX          = h[24];
    pktBarY          = h[25];
    pktBarW          = h[26];
    pktBarColor      = ((uint16_t)h[27] << 8) | h[28];
    pktClockFlags    = h[29];
    pktClockEpochSec = ((uint32_t)h[30] << 24)
                     | ((uint32_t)h[31] << 16)
                     | ((uint32_t)h[32] <<  8)
                     |            h[33];
    pktClockTzMin    = (int16_t)(((uint16_t)h[34] << 8) | h[35]);
    pktClockFontId   = h[36];
    pktClockOffsetX  = (int8_t)h[37];
    pktClockOffsetY  = (int8_t)h[38];
    // h[39] reserved
    pktHoursColor    = ((uint16_t)h[40] << 8) | h[41];
    pktMinutesColor  = ((uint16_t)h[42] << 8) | h[43];
    pktSecondsColor  = ((uint16_t)h[44] << 8) | h[45];
    pktColonColor    = ((uint16_t)h[46] << 8) | h[47];
    pktDateColor     = ((uint16_t)h[48] << 8) | h[49];
    pktAmpmColor     = ((uint16_t)h[50] << 8) | h[51];
}

// ─────────────────────────────────────────────────────────────────────────────
// processPacket
// ─────────────────────────────────────────────────────────────────────────────

static void processPacket() {
    const uint32_t headerAndPayload = HEADER_SIZE + pktPayloadBytes;

    const uint16_t storedCrc =
        ((uint16_t)pktBuf[pendingBuf][headerAndPayload] << 8)
        | pktBuf[pendingBuf][headerAndPayload + 1];
    const uint16_t computedCrc = crc16(pktBuf[pendingBuf], headerAndPayload);

    if (storedCrc != computedCrc) {
        Serial.printf("[NAK] CRC fail — stored=0x%04X computed=0x%04X\n",
                      storedCrc, computedCrc);
        Serial.write(RESP_NAK);
        Serial.flush();
        return;
    }

    const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;
    if (pktWidth != PANEL_WIDTH || pktHeight != REAL_HEIGHT ||
        pktFrameCount == 0 || pktFrameCount > MAX_FRAMES ||
        pktPayloadBytes != expectedPayload) {
        Serial.printf("[ERR] Invalid packet — w=%d h=%d fc=%d pb=%lu\n",
                      pktWidth, pktHeight, pktFrameCount,
                      (unsigned long)pktPayloadBytes);
        Serial.write(RESP_ERR);
        Serial.flush();
        return;
    }

    if (pktFlags == FRM_NEXT) {
        xSemaphoreTake(nextMutex, portMAX_DELAY);
        memcpy(nextBuf, pktBuf[pendingBuf], HEADER_SIZE + pktPayloadBytes + CRC_SIZE);
        nextFrameCount = pktFrameCount;
        nextFrameDurMs = (pktDurMs > 0) ? pktDurMs : 100;
        nextStartPosMs = pktStartPosMs;
        nextTrackDurMs = pktTrackDurMs;
        nextBarX = pktBarX; nextBarY = pktBarY; nextBarW = pktBarW;
        nextBarColor = pktBarColor;
        nextBufReady = true;
        xSemaphoreGive(nextMutex);
        Serial.printf("[NEXT] Preloaded: %d frames\n", pktFrameCount);
        Serial.write(RESP_ACK);
        Serial.flush();
        return;
    }

    // Normal commit
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    activeBuf           = pendingBuf;
    activeFrameCount    = pktFrameCount;
    activeFrameDurMs    = (pktDurMs > 0) ? pktDurMs : 100;
    activeStartPosMs    = pktStartPosMs;
    activeTrackDurMs    = pktTrackDurMs;
    commitTimeMs        = millis();
    activeBarX          = pktBarX;
    activeBarY          = pktBarY;
    activeBarW          = pktBarW;
    activeBarColor      = pktBarColor;
    activeClockFlags    = pktClockFlags;
    activeClockEpochSec = pktClockEpochSec;
    activeClockTzMin    = pktClockTzMin;
    activeClockFontId   = pktClockFontId;
    activeClockOffsetX  = pktClockOffsetX;
    activeClockOffsetY  = pktClockOffsetY;
    activeHoursColor    = pktHoursColor;
    activeMinutesColor  = pktMinutesColor;
    activeSecondsColor  = pktSecondsColor;
    activeColonColor    = pktColonColor;
    activeDateColor     = pktDateColor;
    activeAmpmColor     = pktAmpmColor;
    xSemaphoreGive(swapMutex);

    pendingBuf = 1 - pendingBuf;

    Serial.printf("[ACK] %d frames @ %d ms/frame  clock=%s font=%d epochSec=%lu tzMin=%d\n",
                  pktFrameCount, pktDurMs,
                  (pktClockFlags & CLK_FLAG_PRESENT) ? "yes" : "no",
                  pktClockFontId,
                  (unsigned long)pktClockEpochSec,
                  (int)pktClockTzMin);
    Serial.write(RESP_ACK);
    Serial.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial — receive state machine, Core 1
// ─────────────────────────────────────────────────────────────────────────────

static void processSerial() {
    uint8_t* buf = pktBuf[pendingBuf];

    while (rxState == RX_IDLE && Serial.available()) {
        const uint8_t b = (uint8_t)Serial.read();
        syncBuf[0] = syncBuf[1]; syncBuf[1] = syncBuf[2]; syncBuf[2] = b;
        if (syncBuf[0] == FRM_MAGIC_0 &&
            syncBuf[1] == FRM_MAGIC_1 &&
            syncBuf[2] == FRM_MAGIC_2) {
            buf[0] = FRM_MAGIC_0; buf[1] = FRM_MAGIC_1; buf[2] = FRM_MAGIC_2;
            rxIdx   = 3;
            rxState = RX_HEADER;
            memset(syncBuf, 0, sizeof(syncBuf));
            Serial.println("[RX]  Magic found — reading header...");
        }
    }

    if (rxState == RX_HEADER && Serial.available()) {
        const size_t needed = HEADER_SIZE - rxIdx;
        const size_t avail  = (size_t)Serial.available();
        const size_t toRead = (needed < avail) ? needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;

        if (rxIdx == HEADER_SIZE) {
            parseHeader();
            const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;
            const bool ok = (pktWidth == PANEL_WIDTH)
                         && (pktHeight == REAL_HEIGHT)
                         && (pktFrameCount > 0)
                         && (pktFrameCount <= MAX_FRAMES)
                         && (pktPayloadBytes == expectedPayload)
                         && (HEADER_SIZE + pktPayloadBytes + CRC_SIZE <= MAX_PACKET)
                         && (pktFlags == FRM_VERSION || pktFlags == FRM_NEXT);
            if (!ok) {
                Serial.printf("[ERR] Header invalid\n");
                Serial.write(RESP_ERR); Serial.flush();
                rxState = RX_IDLE; rxIdx = 0;
            } else {
                rxState = RX_PAYLOAD;
                Serial.printf("[RX]  Header OK — %d frames %lu B %d ms/frame\n",
                              pktFrameCount, (unsigned long)pktPayloadBytes, pktDurMs);
            }
        }
    }

    if (rxState == RX_PAYLOAD && Serial.available()) {
        const uint32_t payloadEnd = HEADER_SIZE + pktPayloadBytes;
        const uint32_t needed     = payloadEnd - rxIdx;
        const size_t   avail      = (size_t)Serial.available();
        const size_t   toRead     = (needed < (uint32_t)avail) ? needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;
        if (rxIdx == payloadEnd) rxState = RX_CRC_H;
    }

    if (rxState == RX_CRC_H && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        rxState = RX_CRC_L;
    }

    if (rxState == RX_CRC_L && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        processPacket();
        rxState = RX_IDLE; rxIdx = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// displayTask — Core 0
// ─────────────────────────────────────────────────────────────────────────────

static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        xSemaphoreTake(swapMutex, portMAX_DELAY);
        const int      buf        = activeBuf;
        const int      count      = activeFrameCount;
        const uint16_t dur        = activeFrameDurMs;
        const uint32_t startPos   = activeStartPosMs;
        const uint32_t trackDur   = activeTrackDurMs;
        const uint32_t committed  = commitTimeMs;
        const uint8_t  barX       = activeBarX;
        const uint8_t  barY       = activeBarY;
        const uint8_t  barW       = activeBarW;
        const uint16_t barColor   = activeBarColor;
        const uint8_t  clockFlags = activeClockFlags;
        const uint32_t epochSec   = activeClockEpochSec;
        const int16_t  tzMin      = activeClockTzMin;
        const uint8_t  clockFont  = activeClockFontId;
        const int8_t   clockOffX  = activeClockOffsetX;
        const int8_t   clockOffY  = activeClockOffsetY;
        const uint16_t hoursCol   = activeHoursColor;
        const uint16_t minutesCol = activeMinutesColor;
        const uint16_t secondsCol = activeSecondsColor;
        const uint16_t colonCol   = activeColonColor;
        const uint16_t dateCol    = activeDateColor;
        const uint16_t ampmCol    = activeAmpmColor;
        xSemaphoreGive(swapMutex);

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            matrix->flipDMABuffer();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        const uint32_t now      = millis();
        const uint32_t wallMs   = now - committed;
        const uint32_t songPosMs = startPos + wallMs;

        // Next-song preload check (Spotify)
        if (trackDur > 0 && songPosMs >= trackDur - 10000UL) {
            xSemaphoreTake(nextMutex, portMAX_DELAY);
            const bool hasNext = nextBufReady;
            xSemaphoreGive(nextMutex);
            if (hasNext) {
                xSemaphoreTake(nextMutex, portMAX_DELAY);
                memcpy(pktBuf[pendingBuf], nextBuf, MAX_PACKET);
                const int      nCount = nextFrameCount;
                const uint16_t nDur   = nextFrameDurMs;
                const uint32_t nStart = nextStartPosMs;
                const uint32_t nTrack = nextTrackDurMs;
                nextBufReady = false;
                xSemaphoreGive(nextMutex);
                xSemaphoreTake(swapMutex, portMAX_DELAY);
                activeBuf        = pendingBuf;
                activeFrameCount = nCount;
                activeFrameDurMs = nDur;
                activeStartPosMs = nStart;
                activeTrackDurMs = nTrack;
                commitTimeMs     = millis();
                activeBarX = nextBarX; activeBarY = nextBarY;
                activeBarW = nextBarW; activeBarColor = nextBarColor;
                xSemaphoreGive(swapMutex);
                pendingBuf   = 1 - pendingBuf;
                currentFrame = 0;
                Serial.println("[NEXT] Next-song preload activated.");
                continue;
            }
        }

        const uint32_t t0 = millis();

        // 1. Render background frame
        renderFrame(buf, currentFrame);

        // 2. Overdraw progress bar (Spotify)
        overdrawProgressBar(songPosMs, trackDur, barX, barY, barW, barColor);

        // 3. Overdraw clock with correct font
        if (clockFlags & CLK_FLAG_PRESENT) {
            overdrawClock(epochSec, wallMs, tzMin, clockFlags,
                          clockFont, clockOffX, clockOffY,
                          hoursCol, minutesCol, secondsCol,
                          colonCol, dateCol, ampmCol);
        }

        // 4. Commit all three layers atomically
        matrix->flipDMABuffer();

        const uint32_t elapsed  = millis() - t0;
        currentFrame = (currentFrame + 1) % count;
        const int32_t remaining = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) vTaskDelay(pdMS_TO_TICKS(remaining));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(921600);
    delay(200);
    Serial.println("\n\nFrameon Firmware v1.6");
    Serial.println("════════════════════════════════════════");

    for (int i = 0; i < 2; i++) {
        pktBuf[i] = (uint8_t*)ps_malloc(MAX_PACKET);
        if (!pktBuf[i]) {
            Serial.printf("FATAL: ps_malloc failed for pktBuf[%d]\n", i);
            while (true) { delay(500); Serial.print('!'); }
        }
        memset(pktBuf[i], 0, MAX_PACKET);
    }

    nextBuf = (uint8_t*)ps_malloc(MAX_PACKET);
    if (!nextBuf) {
        Serial.println("FATAL: ps_malloc failed for nextBuf");
        while (true) { delay(500); Serial.print('!'); }
    }
    memset(nextBuf, 0, MAX_PACKET);

    Serial.printf("PSRAM buffers OK (3 x %lu KB).  Free: %lu KB\n",
                  (unsigned long)(MAX_PACKET / 1024),
                  (unsigned long)(ESP.getFreePsram() / 1024));

    swapMutex = xSemaphoreCreateMutex();
    nextMutex = xSemaphoreCreateMutex();

    Serial.println("Initialising matrix...");

    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN);
    mxconfig.gpio.r1  = PIN_R1;  mxconfig.gpio.g1  = PIN_G1;
    mxconfig.gpio.b1  = PIN_B1;  mxconfig.gpio.r2  = PIN_R2;
    mxconfig.gpio.g2  = PIN_G2;  mxconfig.gpio.b2  = PIN_B2;
    mxconfig.gpio.a   = PIN_A;   mxconfig.gpio.b   = PIN_B;
    mxconfig.gpio.c   = PIN_C;   mxconfig.gpio.d   = PIN_D;
    mxconfig.gpio.e   = PIN_E;   mxconfig.gpio.clk = PIN_CLK;
    mxconfig.gpio.lat = PIN_LAT; mxconfig.gpio.oe  = PIN_OE;
    mxconfig.driver      = HUB75_I2S_CFG::SHIFTREG;
    mxconfig.clkphase    = false;
    mxconfig.double_buff = true;

    matrix = new MatrixPanel_I2S_DMA(mxconfig);
    if (!matrix->begin()) {
        Serial.println("FATAL: matrix->begin() failed.");
        while (true) { delay(500); Serial.print('.'); }
    }
    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    Serial.println("Matrix OK.");

    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 0);

    Serial.println("Ready.");
    Serial.println("────────────────────────────────────────");
    Serial.printf("  Protocol:    v1.5 (header %d B)\n", HEADER_SIZE);
    Serial.printf("  Panel:       %dx%d\n", PANEL_WIDTH, REAL_HEIGHT);
    Serial.printf("  Max frames:  %d  (%lu KB max payload)\n",
                  MAX_FRAMES, (unsigned long)(MAX_PAYLOAD / 1024));
    Serial.printf("  Clock fonts: 7 (Polymorph…Phantasm)\n");
    Serial.println("  Waiting for Frameon packets on USB Serial...");
    Serial.println("────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    processSerial();
    vTaskDelay(1);
}