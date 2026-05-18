// ─────────────────────────────────────────────────────────────────────────────
// clockhelper.cpp — Live clock renderer (v1.6)
//
// Manages wall-time derivation and dispatches to the six per-layout renderers.
// All glyph data and drawing helpers live in fonthelper.cpp / fonthelper.h.
//
// Called exclusively from Core 0 (displayTask). Never call from Core 1.
// ─────────────────────────────────────────────────────────────────────────────

#include "clockhelper.h"
#include "fonthelper.h"
#include "frameon.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include <stdio.h>

extern MatrixPanel_I2S_DMA* matrix;


// ═════════════════════════════════════════════════════════════════════════════
// TIME DECOMPOSITION
// ═════════════════════════════════════════════════════════════════════════════

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
    while (m <= 12 && days >= kDaysInMonth[leap][m - 1]) {
        days -= kDaysInMonth[leap][m - 1];
        m++;
    }
    ct.month = m;
    ct.day   = (int)days + 1;
    return ct;
}

// Zeller's congruence — returns 0=MON .. 6=SUN.
static int dayOfWeek(int year, int month, int day) {
    int y = year, m = month;
    if (m < 3) { m += 12; y -= 1; }
    int K = y % 100, J = y / 100;
    int h = (day + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    static const int remap[7] = {5, 6, 0, 1, 2, 3, 4};
    return remap[h];
}

static const char* kWeekdayShort[7] = {
    "MON", "TUE", "WED", "THU", "FRI", "SAT", "SUN"
};


// ═════════════════════════════════════════════════════════════════════════════
// PER-STYLE RENDERERS — forward declarations
// ═════════════════════════════════════════════════════════════════════════════

static void clockClassic(const ClockTime& ct, uint32_t wallMs, uint8_t flags,
    int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t sCol,
    uint16_t cCol, uint16_t dCol, uint16_t aCol);

static void clockAnalog(const ClockTime& ct, uint8_t analogFlags,
    int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t sCol,
    uint16_t cCol, uint16_t faceCol, uint16_t rimCol,
    bool h12, bool blink, uint32_t wallMs);

static void clockWeekdayPrefix(const ClockTime& ct, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t lblCol);

static void clockStacked(const ClockTime& ct, bool h12,
    int8_t offX, int8_t offY, uint16_t hCol, uint16_t mCol);

static void clockSecondsBar(const ClockTime& ct, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t barCol);

static void clockDualTz(const ClockTime& ct1, const ClockTime& ct2,
    const char* lbl1, const char* lbl2, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t lblCol);


// ═════════════════════════════════════════════════════════════════════════════
// MAIN ENTRY POINT
// ═════════════════════════════════════════════════════════════════════════════

void overdrawClock(
    uint32_t    epochSec,
    uint32_t    wallMs,
    int16_t     tzOffsetMin,
    int16_t     tz2OffsetMin,
    uint8_t     flags,
    uint8_t     layoutStyle,
    uint8_t     analogFlags,
    uint8_t     fontId,
    int8_t      offX,
    int8_t      offY,
    uint16_t    hoursCol,
    uint16_t    minutesCol,
    uint16_t    secondsCol,
    uint16_t    colonCol,
    uint16_t    dateCol,
    uint16_t    ampmCol,
    const char* label1,
    const char* label2)
{
    if (!(flags & CLK_FLAG_PRESENT)) return;

    fontSetActive(fontId); // sets gActiveFont + gActiveAlphabet

    const uint32_t  wallSec    = epochSec + wallMs / 1000;
    const ClockTime ct         = epochToTime(wallSec, tzOffsetMin);
    const bool      h12        = flags & CLK_FLAG_H12;
    const bool      blinkColon = flags & CLK_FLAG_BLINK;

    switch (layoutStyle) {
        case CLK_LAYOUT_ANALOG:
            clockAnalog(ct, analogFlags, offX, offY,
                hoursCol, minutesCol, secondsCol, colonCol, dateCol, ampmCol,
                h12, blinkColon, wallMs);
            break;

        case CLK_LAYOUT_WEEKDAY_PREFIX:
            clockWeekdayPrefix(ct, wallMs, h12, blinkColon,
                offX, offY, hoursCol, minutesCol, colonCol, ampmCol);
            break;

        case CLK_LAYOUT_STACKED:
            clockStacked(ct, h12, offX, offY, hoursCol, minutesCol);
            break;

        case CLK_LAYOUT_SECONDS_BAR:
            clockSecondsBar(ct, wallMs, h12, blinkColon,
                offX, offY, hoursCol, minutesCol, colonCol, secondsCol);
            break;

        case CLK_LAYOUT_DUAL_TIMEZONE: {
            const ClockTime ct2 = epochToTime(wallSec, tz2OffsetMin);
            clockDualTz(ct, ct2, label1, label2, wallMs, h12, blinkColon,
                offX, offY, hoursCol, minutesCol, colonCol, ampmCol);
            break;
        }

        case CLK_LAYOUT_CLASSIC:
        default:
            clockClassic(ct, wallMs, flags, offX, offY,
                hoursCol, minutesCol, secondsCol, colonCol, dateCol, ampmCol);
            break;
    }
}


// ═════════════════════════════════════════════════════════════════════════════
// PER-STYLE RENDERER IMPLEMENTATIONS
// ═════════════════════════════════════════════════════════════════════════════

// ── Classic ───────────────────────────────────────────────────────────────────
static void clockClassic(const ClockTime& ct, uint32_t wallMs, uint8_t flags,
    int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t sCol,
    uint16_t cCol, uint16_t dCol, uint16_t aCol)
{
    const bool h12      = flags & CLK_FLAG_H12;
    const bool showSec  = flags & CLK_FLAG_SECONDS;
    const bool showDate = flags & CLK_FLAG_DATE;
    const bool doBlink  = flags & CLK_FLAG_BLINK;
    const bool showAmPm = flags & CLK_FLAG_AMPM;
    const bool colonVis = !doBlink || (wallMs % 1000) < 500;

    char hBuf[4], mBuf[4], sBuf[4], amBuf[4], dateBuf[10];
    int dispHour = ct.hour;
    if (h12) {
        dispHour = ct.hour % 12;
        if (!dispHour) dispHour = 12;
        snprintf(hBuf, sizeof(hBuf), "%d",  dispHour);
        snprintf(amBuf, sizeof(amBuf), "%s", ct.hour < 12 ? "AM" : "PM");
    } else {
        snprintf(hBuf, sizeof(hBuf), "%02d", dispHour);
        amBuf[0] = '\0';
    }
    snprintf(mBuf,    sizeof(mBuf),    "%02d", ct.minute);
    snprintf(sBuf,    sizeof(sBuf),    "%02d", ct.second);
    snprintf(dateBuf, sizeof(dateBuf), "%02d.%02d.%02d",
             ct.day, ct.month, ct.year % 100);

    const int colonW = gActiveFont[glyphIndex(':')].w;
    int timeW = textWidth(hBuf) + 2 + colonW + textWidth(mBuf);
    if (showSec)  timeW += 2 + colonW + textWidth(sBuf);
    if (showAmPm) timeW += 1 + textWidth(amBuf);

    const int charH  = 7;
    const int totalH = charH + (showDate ? charH + 2 : 0);
    const int startY = (REAL_HEIGHT - totalH) / 2 + offY;
    int cx = (PANEL_WIDTH - timeW) / 2 + offX;

    drawText(hBuf, cx, startY, hCol);               cx += textWidth(hBuf) + 2;
    if (colonVis) drawGlyph(':', cx - 1, startY, cCol);
    cx += colonW;
    drawText(mBuf, cx, startY, mCol);               cx += textWidth(mBuf);

    if (showSec) {
        cx += 2;
        if (colonVis) drawGlyph(':', cx - 1, startY, cCol);
        cx += colonW;
        drawText(sBuf, cx, startY, sCol);           cx += textWidth(sBuf);
    }
    if (showAmPm && amBuf[0]) {
        cx += 1;
        drawText(amBuf, cx, startY, aCol);
    }
    if (showDate) {
        const int dw = textWidth(dateBuf);
        drawText(dateBuf, (PANEL_WIDTH - dw) / 2 + offX, startY + charH + 2, dCol);
    }
}

// ── Analog ────────────────────────────────────────────────────────────────────
static void clockAnalog(const ClockTime& ct, uint8_t analogFlags,
    int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t sCol,
    uint16_t cCol, uint16_t faceCol, uint16_t rimCol,
    bool h12, bool blink, uint32_t wallMs)
{
    const uint8_t faceStyle   = analogFlags & ANALOG_FACE_MASK;
    const bool    showSecHand = analogFlags & ANALOG_SHOW_SECOND_HAND;
    const bool    showDigital = analogFlags & ANALOG_SHOW_DIGITAL;

    // Half-pixel centre (cx+0.5, cy+0.5) with radius 13.5 gives exactly
    // 2 px margin on all four sides of the 32-row panel:
    //   round(15.5 - 13.5) = 2,  round(15.5 + 13.5) = 29  → 2 px bottom.
    const float radius = 13.5f;
    const float faceCx = (float)(showDigital ? 15 : (PANEL_WIDTH / 2 - 1)) + offX + 0.5f;
    const float faceCy = (float)(REAL_HEIGHT / 2 - 1) + offY + 0.5f;

    // Parametric circle — float centre/radius, round each pixel position.
    {
        const int steps = (int)(2.0f * (float)M_PI * radius * 2.0f) + 8;
        for (int i = 0; i < steps; i++) {
            float angle = 2.0f * (float)M_PI * i / steps;
            int px = (int)(faceCx + radius * sinf(angle) + 0.5f);
            int py = (int)(faceCy - radius * cosf(angle) + 0.5f);
            if (px >= 0 && px < PANEL_WIDTH && py >= 0 && py < REAL_HEIGHT)
                matrix->drawPixel(px, py, rimCol);
        }
    }

    auto dotAt = [&](int hour) {
        const float r     = hour * 30.0f * (float)M_PI / 180.0f;
        const float inner = radius - 2.0f;
        const float fx    = faceCx + inner * sinf(r);
        const float fy    = faceCy - inner * cosf(r);

        if (hour == 0 || hour == 6) {
            // 2×1 horizontal — two adjacent X pixels at the same Y
            const int y = (int)(fy + 0.5f);
            matrix->drawPixel((int)floorf(fx), y, faceCol);
            matrix->drawPixel((int)ceilf(fx),  y, faceCol);
        } else if (hour == 3 || hour == 9) {
            // 1×2 vertical — two adjacent Y pixels at the same X
            const int x = (int)(fx + 0.5f);
            matrix->drawPixel(x, (int)floorf(fy), faceCol);
            matrix->drawPixel(x, (int)ceilf(fy),  faceCol);
        } else {
            // Non-cardinal hours — single pixel
            matrix->drawPixel((int)(fx + 0.5f), (int)(fy + 0.5f), faceCol);
        }
    };
    auto tickAt = [&](int hour) {
        // 2x2-pixel tick: 2 px deep from rim, 2 px wide (perpendicular)
        const float r  = hour * 30.0f * (float)M_PI / 180.0f;
        const int   dx = (int)(cosf(r) + 0.5f);
        const int   dy = (int)(sinf(r) + 0.5f);
        for (int depth = 0; depth <= 1; depth++) {
            const int x = (int)(faceCx + (radius - depth) * sinf(r) + 0.5f);
            const int y = (int)(faceCy - (radius - depth) * cosf(r) + 0.5f);
            matrix->drawPixel(x,      y,      faceCol);
            matrix->drawPixel(x + dx, y + dy, faceCol);
        }
    };
    switch (faceStyle) {
        case ANALOG_FACE_CARDINAL:
            for (int h : {0,3,6,9}) dotAt(h);  break;
        case ANALOG_FACE_ALL_DOTS:
            for (int h = 0; h < 12; h++) dotAt(h);  break;
        case ANALOG_FACE_TICKS:
            for (int h : {0,3,6,9}) tickAt(h);  break;
        default: break;
    }

    const float minFrac = ct.minute / 60.0f;
    const float secFrac = ct.second / 60.0f;
    auto handTo = [&](float deg, int len, uint16_t col) {
        const float r  = deg * (float)M_PI / 180.0f;
        const int   cx = (int)faceCx;
        const int   cy = (int)faceCy;
        matrix->drawLine(cx, cy,
            (int)(faceCx + len * sinf(r) + 0.5f),
            (int)(faceCy - len * cosf(r) + 0.5f), col);
    };
    handTo(((ct.hour % 12) + minFrac) * 30.0f, radius - 7, hCol);
    handTo((ct.minute + secFrac) * 6.0f,        radius - 2, mCol);
    if (showSecHand) handTo(ct.second * 6.0f,   radius - 1, sCol);
    matrix->drawPixel((int)faceCx, (int)faceCy, hCol);

    if (showDigital) {
        const bool cv = !blink || (wallMs % 1000) < 500;
        char hBuf[4], mBuf[4];
        int dh = ct.hour;
        if (h12) { dh = dh % 12; if (!dh) dh = 12; snprintf(hBuf, 4, "%d",  dh); }
        else                                          snprintf(hBuf, 4, "%02d", dh);
        snprintf(mBuf, 4, "%02d", ct.minute);

        const int colonW = gActiveFont[glyphIndex(':')].w;
        const int timeW  = textWidth(hBuf) + 2 + colonW + textWidth(mBuf);
        const int xStart = 32 + (32 - timeW) / 2 + offX;
        const int y      = (REAL_HEIGHT - 7) / 2 + offY;

        int cx = xStart;
        drawText(hBuf, cx, y, hCol);   cx += textWidth(hBuf) + 2;
        if (cv) drawGlyph(':', cx - 1, y, cCol);
        cx += colonW;
        drawText(mBuf, cx, y, mCol);
    }
}

// ── Weekday prefix ────────────────────────────────────────────────────────────
static void clockWeekdayPrefix(const ClockTime& ct, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t lblCol)
{
    const bool cv = !blink || (wallMs % 1000) < 500;
    const char* wd = kWeekdayShort[dayOfWeek(ct.year, ct.month, ct.day)];

    char hBuf[4], mBuf[4];
    int dh = ct.hour;
    if (h12) { dh = dh % 12; if (!dh) dh = 12; snprintf(hBuf, 4, "%d",  dh); }
    else                                          snprintf(hBuf, 4, "%02d", dh);
    snprintf(mBuf, 4, "%02d", ct.minute);

    const int colonW = gActiveFont[glyphIndex(':')].w;
    const int wdW    = labelWidth(wd);
    const int timeW  = textWidth(hBuf) + 2 + colonW + textWidth(mBuf);
    const int y      = (REAL_HEIGHT - 7) / 2 + offY;
    int cx           = (PANEL_WIDTH - (wdW + 2 + timeW)) / 2 + offX;

    drawLabel(wd,   cx, y, lblCol); cx += wdW + 2;
    drawText(hBuf,  cx, y, hCol);   cx += textWidth(hBuf) + 2;
    if (cv) drawGlyph(':', cx - 1, y, cCol);
    cx += colonW;
    drawText(mBuf, cx, y, mCol);
}

// ── Stacked ───────────────────────────────────────────────────────────────────
static void clockStacked(const ClockTime& ct, bool h12,
    int8_t offX, int8_t offY, uint16_t hCol, uint16_t mCol)
{
    char hBuf[4], mBuf[4];
    int dh = ct.hour;
    if (h12) { dh = dh % 12; if (!dh) dh = 12; snprintf(hBuf, 4, "%d",  dh); }
    else                                          snprintf(hBuf, 4, "%02d", dh);
    snprintf(mBuf, 4, "%02d", ct.minute);

    const int totalH = 7 * 2 + 2;
    const int startY = (REAL_HEIGHT - totalH) / 2 + offY;
    drawText(hBuf, (PANEL_WIDTH - textWidth(hBuf)) / 2 + offX, startY,     hCol);
    drawText(mBuf, (PANEL_WIDTH - textWidth(mBuf)) / 2 + offX, startY + 9, mCol);
}

// ── Seconds bar ───────────────────────────────────────────────────────────────
static void clockSecondsBar(const ClockTime& ct, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t barCol)
{
    const bool cv = !blink || (wallMs % 1000) < 500;

    char hBuf[4], mBuf[4];
    int dh = ct.hour;
    if (h12) { dh = dh % 12; if (!dh) dh = 12; snprintf(hBuf, 4, "%d",  dh); }
    else                                          snprintf(hBuf, 4, "%02d", dh);
    snprintf(mBuf, 4, "%02d", ct.minute);

    const int colonW = gActiveFont[glyphIndex(':')].w;
    const int timeW  = textWidth(hBuf) + 2 + colonW + textWidth(mBuf);
    const int totalH = 7 + 2 + 2;
    const int startY = (REAL_HEIGHT - totalH) / 2 + offY;
    const int barY   = startY + 7 + 2;
    int cx           = (PANEL_WIDTH - timeW) / 2 + offX;

    drawText(hBuf, cx, startY, hCol);   cx += textWidth(hBuf) + 2;
    if (cv) drawGlyph(':', cx - 1, startY, cCol);
    cx += colonW;
    drawText(mBuf, cx, startY, mCol);

    const int barW   = 50; 
    const int barX   = (PANEL_WIDTH - barW) / 2 + offX;//
    
    // Use RGB(64,64,64) - Dark grey for better visibility of colored fill
    const uint16_t greyTrack = 0x3186; // RGB565 for (64,64,64)
    
    // Draw entire bar in dark grey (un-filled background)
    matrix->fillRect(barX, barY, barW, 2, greyTrack);
    
    // Draw colored fill for elapsed seconds
    const int filled = (barW * ct.second) / 60;
    if (filled > 0) matrix->fillRect(barX, barY, filled, 2, barCol);
}

// ── Dual timezone ─────────────────────────────────────────────────────────────
static void clockDualTz(const ClockTime& ct1, const ClockTime& ct2,
    const char* lbl1, const char* lbl2, uint32_t wallMs,
    bool h12, bool blink, int8_t offX, int8_t offY,
    uint16_t hCol, uint16_t mCol, uint16_t cCol, uint16_t lblCol)
{
    const bool cv    = !blink || (wallMs % 1000) < 500;
    const int totalH = 7 * 2 + 2;
    const int startY = (REAL_HEIGHT - totalH) / 2 + offY;

    auto drawRow = [&](const char* label, const ClockTime& ct, int y) {
        char hBuf[4], mBuf[4];
        int dh = ct.hour;
        if (h12) { dh = dh % 12; if (!dh) dh = 12; snprintf(hBuf, 4, "%d",  dh); }
        else                                          snprintf(hBuf, 4, "%02d", dh);
        snprintf(mBuf, 4, "%02d", ct.minute);

        const int colonW = gActiveFont[glyphIndex(':')].w;
        const int lblW   = labelWidth(label);
        const int timeW  = textWidth(hBuf) + 2 + colonW + textWidth(mBuf);
        int cx = (PANEL_WIDTH - (lblW + 2 + timeW)) / 2 + offX;

        drawLabel(label, cx, y, lblCol); cx += lblW + 2;
        drawText(hBuf,   cx, y, hCol);   cx += textWidth(hBuf) + 2;
        if (cv) drawGlyph(':', cx - 1, y, cCol);
        cx += colonW;
        drawText(mBuf, cx, y, mCol);
    };

    drawRow(lbl1, ct1, startY);
    drawRow(lbl2, ct2, startY + 9);
}