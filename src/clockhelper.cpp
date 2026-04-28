// ─────────────────────────────────────────────────────────────────────────────
// clockhelper.cpp — Live clock overdraw renderer (v1.5)
//
// Derives current wall time from a Unix epoch timestamp baked into the packet
// header at commit time, plus millis() elapsed since commit:
//
//     wallSec = epochSec + wallMs / 1000
//
// Renders hours, minutes, optional seconds, optional date row, and optional
// AM/PM label using a compact 7-row bitmap font matching Frameon's Polymorph
// style. Each element has its own RGB565 colour from the packet header.
//
// Font note: fontId is accepted but currently unused — all fontIds render the
// same built-in bitmap glyphs. Multi-font support is reserved for a future
// firmware update.
// ─────────────────────────────────────────────────────────────────────────────

#include "clockhelper.h"
#include "frameon.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <stdio.h>

// Shared matrix handle — defined in main.cpp.
extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// Compact bitmap font
//
// Glyph format: { pixelWidth, { row0..row6 } }  MSB = leftmost pixel.
// Characters covered: '0'–'9', ':', '.', 'A', 'M', 'P', ' '
// ─────────────────────────────────────────────────────────────────────────────

struct Glyph { uint8_t w; uint8_t rows[7]; };

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
    return 15; // space / fallback
}

// Width of glyph + 1 px inter-character gap.
static int glyphWidth(char c) {
    return kGlyphs[glyphIndex(c)].w + 1;
}

// Total pixel width of a null-terminated string (no trailing gap).
static int strPixelWidth(const char* s) {
    int w = 0;
    while (*s) { w += glyphWidth(*s++); }
    return w > 0 ? w - 1 : 0;
}

static void drawGlyph(char c, int x, int y, uint16_t color) {
    const Glyph& g = kGlyphs[glyphIndex(c)];
    for (int row = 0; row < 7; row++) {
        const uint8_t bits = g.rows[row];
        for (int col = 0; col < (int)g.w; col++) {
            if ((bits >> (g.w - 1 - col)) & 1) {
                matrix->drawPixel(x + col, y + row, color);
            }
        }
    }
}

static void drawStr(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        drawGlyph(*s, x, y, color);
        x += glyphWidth(*s);
        s++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Gregorian calendar decomposition
// ─────────────────────────────────────────────────────────────────────────────

struct ClockTime { int hour, minute, second, day, month, year; };

static const uint8_t kDim[2][12] = {
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
    while (m <= 12 && days >= kDim[leap][m-1]) {
        days -= kDim[leap][m-1];
        m++;
    }
    ct.month = m;
    ct.day   = (int)days + 1;
    return ct;
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawClock
// ─────────────────────────────────────────────────────────────────────────────

void overdrawClock(
    uint32_t epochSec,
    uint32_t wallMs,
    int16_t  tzOffsetMin,
    uint8_t  flags,
    uint8_t  /*fontId*/,
    int8_t   offX,
    int8_t   offY,
    uint16_t hoursCol,
    uint16_t minutesCol,
    uint16_t secondsCol,
    uint16_t colonCol,
    uint16_t dateCol,
    uint16_t ampmCol)
{
    if (!(flags & CLK_FLAG_PRESENT)) return;

    const bool h12      = (flags & CLK_FLAG_H12)      != 0;
    const bool showSec  = (flags & CLK_FLAG_SECONDS)   != 0;
    const bool showDate = (flags & CLK_FLAG_DATE)      != 0;
    const bool blinkCol = (flags & CLK_FLAG_BLINK)     != 0;
    const bool showAmPm = (flags & CLK_FLAG_AMPM)      != 0;

    // Derive current wall time
    const uint32_t wallSec = epochSec + wallMs / 1000;
    const ClockTime ct = epochToTime(wallSec, tzOffsetMin);

    // Blink colon: invisible for the second half of each second
    const bool colonVisible = !blinkCol || (wallMs % 1000) < 500;

    // Build string buffers
    char hBuf[4], mBuf[4], sBuf[4], ampmBuf[4], dateBuf[12];

    if (h12) {
        int dh = ct.hour % 12;
        if (dh == 0) dh = 12;
        snprintf(hBuf,    sizeof(hBuf),    "%d",  dh);
        snprintf(ampmBuf, sizeof(ampmBuf), "%s",  ct.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hBuf,    sizeof(hBuf),    "%02d", ct.hour);
        ampmBuf[0] = '\0';
    }
    snprintf(mBuf,    sizeof(mBuf),    "%02d", ct.minute);
    snprintf(sBuf,    sizeof(sBuf),    "%02d", ct.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%02d",
             ct.day, ct.month, ct.year % 100);

    // ── Measure total time-row width ──────────────────────────────────────
    // Layout mirrors clock_widget.dart spacing exactly:
    //   spacingBeforeColon = 2, colonVisualOffset = -1, spacingAfterColon = 0
    //   spacingGeneral (before AM/PM) = 1

    int timeW = strPixelWidth(hBuf)
              + 2                        // spacingBeforeColon
              + glyphWidth(':')          // colon glyph + gap (gap = 1)
              + strPixelWidth(mBuf);

    if (showSec) {
        timeW += 2                       // spacingBeforeColon
               + glyphWidth(':')
               + strPixelWidth(sBuf);
    }
    if (showAmPm && ampmBuf[0]) {
        timeW += 1 + strPixelWidth(ampmBuf); // spacingGeneral + ampm
    }

    // ── Vertical layout ───────────────────────────────────────────────────
    const int charH  = 7;
    const int totalH = charH + (showDate ? charH + 2 : 0);
    const int startY = (REAL_HEIGHT - totalH) / 2 + (int)offY;
    const int timeY  = startY;
    const int dateY  = startY + charH + 2;

    // ── Draw time row (centered + offset) ─────────────────────────────────
    int cx = (PANEL_WIDTH - timeW) / 2 + (int)offX;

    // Hours
    drawStr(hBuf, cx, timeY, hoursCol);
    cx += strPixelWidth(hBuf);

    // Colon H:M  (spacingBeforeColon=2, colonVisualOffset=-1)
    cx += 2;
    if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
    cx += glyphWidth(':') - 1;   // advance without the -1 visual offset

    // Minutes
    drawStr(mBuf, cx, timeY, minutesCol);
    cx += strPixelWidth(mBuf);

    // Colon M:S + seconds
    if (showSec) {
        cx += 2;
        if (colonVisible) drawGlyph(':', cx - 1, timeY, colonCol);
        cx += glyphWidth(':') - 1;
        drawStr(sBuf, cx, timeY, secondsCol);
        cx += strPixelWidth(sBuf);
    }

    // AM/PM
    if (showAmPm && ampmBuf[0]) {
        cx += 1;
        drawStr(ampmBuf, cx, timeY, ampmCol);
    }

    // ── Draw date row (centered below time) ───────────────────────────────
    if (showDate) {
        const int dw = strPixelWidth(dateBuf);
        const int dx = (PANEL_WIDTH - dw) / 2 + (int)offX;
        drawStr(dateBuf, dx, dateY, dateCol);
    }

    // NOTE: flipDMABuffer() is called by displayTask after this returns.
}   