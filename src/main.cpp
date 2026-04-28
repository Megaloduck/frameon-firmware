/*
 * Frameon Firmware v1.5
 * ESP32-S3  ·  P4-2121-64×32 HUB75E
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
 *
 *   The font renderer is a compact software rasteriser that supports all
 *   7 Frameon fonts using the same 7-row bitmap format as the Dart side.
 *   Only the characters needed for clock display are included:
 *   0-9, ':', '.', 'A', 'M', 'P'  (digits, colon, date dot, AM/PM letters)
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"
#include "waitingscreen.h"

// ─────────────────────────────────────────────────────────────────────────────
// Panel configuration
// ─────────────────────────────────────────────────────────────────────────────

#define PANEL_WIDTH  64
#define PANEL_HEIGHT 64   // physical scan height
#define REAL_HEIGHT  32   // active pixel rows

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
static volatile uint8_t  activeClockFlags    = 0;       // CLK_FLAG_* bitmask
static volatile uint32_t activeClockEpochSec = 0;       // Unix seconds at commit
static volatile int16_t  activeClockTzMin    = 0;       // signed tz offset minutes
static volatile uint8_t  activeClockFontId   = 0;       // 0=Polymorph…6=Phantasm
static volatile int8_t   activeClockOffsetX  = 0;
static volatile int8_t   activeClockOffsetY  = 0;
static volatile uint16_t activeHoursColor    = 0x07E0;
static volatile uint16_t activeMinutesColor  = 0x07E0;
static volatile uint16_t activeSecondsColor  = 0x07E0;
static volatile uint16_t activeColonColor    = 0x07E0;
static volatile uint16_t activeDateColor     = 0x07E0;
static volatile uint16_t activeAmpmColor     = 0x07E0;

// Next-song preload
static uint8_t*          nextBuf = nullptr;             // PSRAM — allocated in setup()
static volatile bool     nextBufReady        = false;
static volatile int      nextFrameCount      = 0;
static volatile uint16_t nextFrameDurMs      = 100;
static volatile uint32_t nextStartPosMs      = 0;
static volatile uint32_t nextTrackDurMs      = 0;
static volatile uint8_t  nextBarX            = 0;
static volatile uint8_t  nextBarY            = 0;
static volatile uint8_t  nextBarW            = 0;
static volatile uint16_t nextBarColor        = 0x11C5;

static SemaphoreHandle_t swapMutex  = nullptr;
static SemaphoreHandle_t nextMutex  = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Serial receive state machine
// ─────────────────────────────────────────────────────────────────────────────

enum RxState : uint8_t { RX_IDLE, RX_HEADER, RX_PAYLOAD, RX_CRC_H, RX_CRC_L };

static RxState   rxState       = RX_IDLE;
static uint32_t  rxIdx         = 0;
static uint8_t   syncBuf[3]    = {0, 0, 0};

static uint8_t   pktFlags         = FRM_VERSION;
static uint16_t  pktFrameCount    = 0;
static uint16_t  pktWidth         = 0;
static uint16_t  pktHeight        = 0;
static uint16_t  pktDurMs         = 0;
static uint32_t  pktPayloadBytes  = 0;
static uint32_t  pktStartPosMs    = 0;
static uint32_t  pktTrackDurMs    = 0;
static uint8_t   pktBarX          = 0;
static uint8_t   pktBarY          = 0;
static uint8_t   pktBarW          = 0;
static uint16_t  pktBarColor      = 0x11C5;
// v1.5 clock fields
static uint8_t   pktClockFlags    = 0;
static uint32_t  pktClockEpochSec = 0;
static int16_t   pktClockTzMin    = 0;
static uint8_t   pktClockFontId   = 0;
static int8_t    pktClockOffsetX  = 0;
static int8_t    pktClockOffsetY  = 0;
static uint16_t  pktHoursColor    = 0x07E0;
static uint16_t  pktMinutesColor  = 0x07E0;
static uint16_t  pktSecondsColor  = 0x07E0;
static uint16_t  pktColonColor    = 0x07E0;
static uint16_t  pktDateColor     = 0x07E0;
static uint16_t  pktAmpmColor     = 0x07E0;

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
// Compact clock font — 7-row bitmaps for 0-9 : . A M P (Polymorph-style)
//
// Each glyph: { width, { row0..row6 } }  MSB = leftmost pixel.
// All fonts on the Dart side are 7 rows tall — we use a single embedded font
// here. Font selection (fontId) is reserved for a future firmware update that
// embeds all 7 fonts; for now all fontIds render the same built-in glyphs.
// ─────────────────────────────────────────────────────────────────────────────

struct Glyph { uint8_t w; uint8_t rows[7]; };

// Characters needed: '0'-'9', ':', '.', 'A', 'M', 'P', ' '
static const Glyph kGlyphs[] = {
    /* '0' */ {4, {0x06,0x09,0x09,0x09,0x09,0x09,0x06}},
    /* '1' */ {2, {0x03,0x01,0x01,0x01,0x01,0x01,0x01}},
    /* '2' */ {4, {0x06,0x09,0x01,0x02,0x04,0x08,0x0F}},
    /* '3' */ {4, {0x06,0x09,0x01,0x06,0x01,0x09,0x06}},
    /* '4' */ {4, {0x01,0x03,0x05,0x09,0x0F,0x01,0x01}},
    /* '5' */ {4, {0x0F,0x08,0x0E,0x01,0x01,0x09,0x06}},
    /* '6' */ {4, {0x06,0x09,0x08,0x0E,0x09,0x09,0x06}},
    /* '7' */ {4, {0x0F,0x01,0x02,0x02,0x04,0x04,0x04}},
    /* '8' */ {4, {0x06,0x09,0x09,0x06,0x09,0x09,0x06}},
    /* '9' */ {4, {0x06,0x09,0x09,0x07,0x01,0x09,0x06}},
    /* ':' */ {1, {0x00,0x01,0x00,0x00,0x00,0x01,0x00}},
    /* '.' */ {1, {0x00,0x00,0x00,0x00,0x00,0x00,0x01}},
    /* 'A' */ {4, {0x06,0x09,0x09,0x0F,0x09,0x09,0x09}},
    /* 'M' */ {5, {0x11,0x1B,0x15,0x11,0x11,0x11,0x11}},
    /* 'P' */ {4, {0x0E,0x09,0x09,0x0E,0x08,0x08,0x08}},
    /* ' ' */ {3, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
};

static int glyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '.') return 11;
    if (c == 'A') return 12;
    if (c == 'M') return 13;
    if (c == 'P') return 14;
    return 15; // space
}

static int glyphWidth(char c) { return kGlyphs[glyphIndex(c)].w + 1; } // +1 gap

static int textWidth(const char* s) {
    int w = 0;
    while (*s) { w += glyphWidth(*s++); }
    return w > 0 ? w - 1 : 0; // no trailing gap
}

static void drawGlyph(char c, int x, int y, uint16_t color) {
    const Glyph& g = kGlyphs[glyphIndex(c)];
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

struct ClockTime {
    int hour, minute, second, day, month, year;
};

// Gregorian calendar: days per month (non-leap / leap indexed by [leap][m-1])
static const uint8_t kDaysInMonth[2][12] = {
    {31,28,31,30,31,30,31,31,30,31,30,31},
    {31,29,31,30,31,30,31,31,30,31,30,31},
};

static bool isLeap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static ClockTime epochToTime(uint32_t epochSec, int16_t tzOffsetMin) {
    // Apply timezone offset before breaking down
    int32_t t = (int32_t)epochSec + (int32_t)tzOffsetMin * 60;
    if (t < 0) t = 0;

    ClockTime ct;
    ct.second = t % 60; t /= 60;
    ct.minute = t % 60; t /= 60;
    ct.hour   = t % 24; t /= 24;

    // Days since epoch (1970-01-01)
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
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawClock(uint32_t epochSec, uint32_t elapsedMs,
                           int16_t tzOffsetMin, uint8_t flags,
                           uint8_t /*fontId*/, int8_t offX, int8_t offY,
                           uint16_t hoursCol,   uint16_t minutesCol,
                           uint16_t secondsCol, uint16_t colonCol,
                           uint16_t dateCol,    uint16_t ampmCol) {

    if (!(flags & CLK_FLAG_PRESENT)) return;

    const bool h12       = flags & CLK_FLAG_H12;
    const bool showSec   = flags & CLK_FLAG_SECONDS;
    const bool showDate  = flags & CLK_FLAG_DATE;
    const bool blinkCol  = flags & CLK_FLAG_BLINK;
    const bool showAmPm  = flags & CLK_FLAG_AMPM;

    // Derive current wall time from epoch + elapsed
    const uint32_t wallSec = epochSec + elapsedMs / 1000;
    ClockTime ct = epochToTime(wallSec, tzOffsetMin);

    // Blink colon: off for the second half of each second
    const bool colonVisible = !blinkCol || (elapsedMs % 1000) < 500;

    // Build time string segments
    char hBuf[4], mBuf[4], sBuf[4], ampmBuf[4], dateBuf[10];

    int dispHour = ct.hour;
    if (h12) {
        dispHour = ct.hour % 12;
        if (dispHour == 0) dispHour = 12;
        snprintf(hBuf,    sizeof(hBuf),    "%d",   dispHour);
        snprintf(ampmBuf, sizeof(ampmBuf), "%s",   ct.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hBuf,    sizeof(hBuf),    "%02d", dispHour);
        ampmBuf[0] = '\0';
    }
    snprintf(mBuf,    sizeof(mBuf),    "%02d", ct.minute);
    snprintf(sBuf,    sizeof(sBuf),    "%02d", ct.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%02d",
             ct.day, ct.month, ct.year % 100);

    // Measure time row width
    int timeW = textWidth(hBuf)
              + 2 + glyphWidth(':')   // spacingBeforeColon=2, afterColon=0
              + textWidth(mBuf);
    if (showSec)   timeW += 2 + glyphWidth(':') + textWidth(sBuf);
    if (showAmPm)  timeW += 1 + textWidth(ampmBuf);

    // Vertical layout
    const int charH     = 7;
    const int totalH    = charH + (showDate ? charH + 2 : 0);
    const int startY    = (REAL_HEIGHT - totalH) / 2 + (int)offY;
    const int timeY     = startY;
    const int dateY     = startY + charH + 2;

    // Horizontal start (centered + offset)
    int cx = (PANEL_WIDTH - timeW) / 2 + (int)offX;

    // Draw hours
    drawText(hBuf, cx, timeY, hoursCol);
    cx += textWidth(hBuf);

    // Draw colon (hours:minutes)
    cx += 2; // spacingBeforeColon
    if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
    cx += glyphWidth(':') - 1; // colonVisualOffset = -1

    // Draw minutes
    drawText(mBuf, cx, timeY, minutesCol);
    cx += textWidth(mBuf);

    // Draw :seconds
    if (showSec) {
        cx += 2;
        if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
        cx += glyphWidth(':') - 1;
        drawText(sBuf, cx, timeY, secondsCol);
        cx += textWidth(sBuf);
    }

    // Draw AM/PM
    if (showAmPm) {
        cx += 1;
        drawText(ampmBuf, cx, timeY, ampmCol);
    }

    // Draw date row (centered, below time)
    if (showDate) {
        int dw = textWidth(dateBuf);
        int dx = (PANEL_WIDTH - dw) / 2 + (int)offX;
        // Draw digit by digit so we can colour '.' in dateColor too
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
    // v1.5 clock fields
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
    // v1.5 clock
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

    Serial.printf("[ACK] %d frames @ %d ms/frame  clock=%s epochSec=%lu tzMin=%d\n",
                  pktFrameCount, pktDurMs,
                  (pktClockFlags & CLK_FLAG_PRESENT) ? "yes" : "no",
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
        const int      buf         = activeBuf;
        const int      count       = activeFrameCount;
        const uint16_t dur         = activeFrameDurMs;
        const uint32_t startPos    = activeStartPosMs;
        const uint32_t trackDur    = activeTrackDurMs;
        const uint32_t committed   = commitTimeMs;
        const uint8_t  barX        = activeBarX;
        const uint8_t  barY        = activeBarY;
        const uint8_t  barW        = activeBarW;
        const uint16_t barColor    = activeBarColor;
        const uint8_t  clockFlags  = activeClockFlags;
        const uint32_t epochSec    = activeClockEpochSec;
        const int16_t  tzMin       = activeClockTzMin;
        const uint8_t  clockFont   = activeClockFontId;
        const int8_t   clockOffX   = activeClockOffsetX;
        const int8_t   clockOffY   = activeClockOffsetY;
        const uint16_t hoursCol    = activeHoursColor;
        const uint16_t minutesCol  = activeMinutesColor;
        const uint16_t secondsCol  = activeSecondsColor;
        const uint16_t colonCol    = activeColonColor;
        const uint16_t dateCol     = activeDateColor;
        const uint16_t ampmCol     = activeAmpmColor;
        xSemaphoreGive(swapMutex);

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            matrix->flipDMABuffer();   // commit waiting screen draw to display
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        const uint32_t now       = millis();
        const uint32_t wallMs    = now - committed;
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

        // 1. Render the background frame
        renderFrame(buf, currentFrame);

        // 2. Overdraw progress bar (Spotify)
        overdrawProgressBar(songPosMs, trackDur, barX, barY, barW, barColor);

        // 3. Overdraw clock (v1.5) — uses live millis()-derived time
        if (clockFlags & CLK_FLAG_PRESENT) {
            overdrawClock(epochSec, wallMs, tzMin, clockFlags,
                          clockFont, clockOffX, clockOffY,
                          hoursCol, minutesCol, secondsCol,
                          colonCol, dateCol, ampmCol);
        }

        // 4. Single flip — commits all three layers atomically
        //matrix->flipDMABuffer();

        const uint32_t elapsed    = millis() - t0;
        currentFrame = (currentFrame + 1) % count;
        const int32_t remaining   = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) vTaskDelay(pdMS_TO_TICKS(remaining));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup / loop
// ─────────────────────────────────────────────────────────────────────────────

// (Next-song buffer — static allocation)
void setup() {
    Serial.begin(921600);

    HUB75_I2S_CFG cfg(PANEL_WIDTH, PANEL_HEIGHT, 1);
    cfg.gpio.e = 18;
    cfg.clkphase = false;
    cfg.driver = HUB75_I2S_CFG::FM6126A;
    matrix = new MatrixPanel_I2S_DMA(cfg);
    matrix->begin();
    matrix->setBrightness8(128);
    matrix->clearScreen();

    pktBuf[0] = (uint8_t*)ps_malloc(MAX_PACKET);
    pktBuf[1] = (uint8_t*)ps_malloc(MAX_PACKET);
    nextBuf   = (uint8_t*)ps_malloc(MAX_PACKET);
    if (!pktBuf[0] || !pktBuf[1] || !nextBuf) {
        Serial.println("[ERR] PSRAM allocation failed");
        while (true) delay(1000);
    }
    memset(pktBuf[0], 0, MAX_PACKET);
    memset(pktBuf[1], 0, MAX_PACKET);
    memset(nextBuf,   0, MAX_PACKET);

    swapMutex = xSemaphoreCreateMutex();
    nextMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(displayTask, "display", 4096, nullptr, 1, nullptr, 0);

    Serial.println("[BOOT] Frameon v1.5 ready");
}

void loop() {
    processSerial();
}   