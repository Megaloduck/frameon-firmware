#include "pomodorohelper.h"
#include "frameon.h"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include <stdio.h>

extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// Polymorph font subset
// Matches polymorph_font.dart used by PomodoroWidget.
// ─────────────────────────────────────────────────────────────────────────────

struct PomoGlyph {
    uint8_t w;
    uint8_t rows[7];
};

static const PomoGlyph kPolymorph[] = {
    // 0
    {5, {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}},
    // 1
    {5, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}},
    // 2
    {5, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}},
    // 3
    {5, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}},
    // 4
    {5, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}},
    // 5
    {5, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}},
    // 6
    {5, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}},
    // 7
    {5, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}},
    // 8
    {5, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}},
    // 9
    {5, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}},

    // :
    {2, {0x00,0x03,0x03,0x00,0x03,0x03,0x00}},

    // blank
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}},

    // B
    {5, {0x1E,0x09,0x09,0x0E,0x09,0x09,0x1E}},
    // C
    {5, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}},
    // E
    {5, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}},
    // F
    {5, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}},
    // K
    {5, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}},
    // O
    {5, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}},
    // R
    {5, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}},
    // S
    {5, {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}},
    // T
    {5, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}},
    // U
    {5, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}},
    // A
    {5, {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}},
};

static const char* kPhaseLabel[3] = {
    "FOCUS",
    "BREAK",
    "REST"
};

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

static int glyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;

    switch (c) {
        case 'B': return 12;
        case 'C': return 13;
        case 'E': return 14;
        case 'F': return 15;
        case 'K': return 16;
        case 'O': return 17;
        case 'R': return 18;
        case 'S': return 19;
        case 'T': return 20;
        case 'U': return 21;
        case 'A': return 22;
        default:  return 11;
    }
}

static int glyphWidth(char c) {
    return kPolymorph[glyphIndex(c)].w + 1;
}

static int textWidth(const char* s) {
    int w = 0;

    while (*s) {
        w += glyphWidth(*s++);
    }

    return w > 0 ? w - 1 : 0;
}

static void drawGlyph(char c, int x, int y, uint16_t color) {
    const PomoGlyph& g = kPolymorph[glyphIndex(c)];

    for (int row = 0; row < 7; row++) {
        uint8_t bits = g.rows[row];

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
        x += glyphWidth(*s++);
    }
}

static void drawTextScale2(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        const PomoGlyph& g = kPolymorph[glyphIndex(*s)];

        for (int row = 0; row < 7; row++) {
            uint8_t bits = g.rows[row];

            for (int col = 0; col < g.w; col++) {
                if ((bits >> (g.w - 1 - col)) & 1) {
                    matrix->drawPixel(x + col * 2,     y + row * 2,     color);
                    matrix->drawPixel(x + col * 2 + 1, y + row * 2,     color);
                    matrix->drawPixel(x + col * 2,     y + row * 2 + 1, color);
                    matrix->drawPixel(x + col * 2 + 1, y + row * 2 + 1, color);
                }
            }
        }

        x += (g.w + 1) * 2;
        s++;
    }
}

static uint16_t dimColor565(uint16_t c, int frac) {
    int r = ((c >> 11) & 0x1F) * frac >> 8;
    int g = ((c >> 5)  & 0x3F) * frac >> 8;
    int b = (c & 0x1F) * frac >> 8;

    return ((uint16_t)r << 11) |
           ((uint16_t)g << 5) |
           ((uint16_t)b);
}

static bool inArc(float angle, float start, float end) {
    float normAngle = fmodf(
        angle - start + 2.0f * (float)M_PI,
        2.0f * (float)M_PI
    );

    float normEnd = fmodf(
        end - start + 2.0f * (float)M_PI,
        2.0f * (float)M_PI
    );

    if (normEnd < 0.001f) {
        return false;
    }

    return normAngle <= normEnd;
}

// ─────────────────────────────────────────────────────────────────────────────
// Split layout
// ─────────────────────────────────────────────────────────────────────────────

static void renderSplit(
    uint32_t liveSec,
    float progress,
    bool colonOn,
    bool showSeconds,
    uint8_t phase,
    int offX,
    int offY,
    uint16_t activeColor)
{
    const int cx = 14;
    const int cy = 16;
    const int outerR = 11;
    const int innerR = 7;

    const uint16_t dimArc = dimColor565(activeColor, 51);

    const float sweepEnd =
        -(float)M_PI / 2.0f +
        progress * 2.0f * (float)M_PI;

    for (int py = cy - outerR; py <= cy + outerR; py++) {
        for (int px = cx - outerR; px <= cx + outerR; px++) {

            float dx = (float)(px - cx);
            float dy = (float)(py - cy);

            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < innerR || dist > outerR) {
                continue;
            }

            float angle = atan2f(dy, dx);

            bool filled = inArc(
                angle,
                -(float)M_PI / 2.0f,
                sweepEnd
            );

            matrix->drawPixel(
                px + offX,
                py + offY,
                filled ? activeColor : dimArc
            );
        }
    }

    char timeBuf[8];

    uint32_t m = (liveSec / 60) % 60;
    uint32_t s = liveSec % 60;

    if (showSeconds) {
        snprintf(
            timeBuf,
            sizeof(timeBuf),
            "%02lu%c%02lu",
            (unsigned long)m,
            colonOn ? ':' : ' ',
            (unsigned long)s
        );
    } else {
        snprintf(
            timeBuf,
            sizeof(timeBuf),
            "%02lu%c00",
            (unsigned long)m,
            colonOn ? ':' : ' '
        );
    }

    const int tw = textWidth(timeBuf);
    const int tx = 46 - tw / 2;
    const int ty = REAL_HEIGHT / 2 - 7 - 1;

    drawText(
        timeBuf,
        tx + offX,
        ty + offY,
        activeColor
    );

    const char* label =
        (phase < 3)
            ? kPhaseLabel[phase]
            : "FOCUS";

    const int lw = textWidth(label);
    const int lx = 46 - lw / 2;
    const int ly = ty + 8;

    drawText(
        label,
        lx + offX,
        ly + offY,
        activeColor
    );
}

// ─────────────────────────────────────────────────────────────────────────────
// Minimalist layout
// ─────────────────────────────────────────────────────────────────────────────

static void renderMinimalist(
    uint32_t liveSec,
    float progress,
    bool showSession,
    uint8_t session,
    uint8_t sessionsTotal,
    int offX,
    int offY,
    uint16_t activeColor)
{
    char mBuf[4];

    snprintf(
        mBuf,
        sizeof(mBuf),
        "%02lu",
        (unsigned long)((liveSec / 60) % 60)
    );

    drawText(
        mBuf,
        1 + offX,
        9 + offY,
        activeColor
    );

    char sBuf[4];

    snprintf(
        sBuf,
        sizeof(sBuf),
        "%02lu",
        (unsigned long)(liveSec % 60)
    );

    const int secondsY =
        REAL_HEIGHT - 14 - 1;

    drawTextScale2(
        sBuf,
        1 + offX,
        secondsY + offY,
        activeColor
    );

    const int barX = 61;
    const int barTop = 1;
    const int barBot = 31;
    const int barH = barBot - barTop;

    const int filled =
        (int)(progress * (float)barH + 0.5f);

    const uint16_t dimBar =
        dimColor565(activeColor, 38);

    for (int y = barTop; y < barBot; y++) {

        bool on =
            y >= (barTop + barH - filled);

        uint16_t c =
            on ? activeColor : dimBar;

        matrix->drawPixel(
            barX + offX,
            y + offY,
            c
        );

        matrix->drawPixel(
            barX + 1 + offX,
            y + offY,
            c
        );
    }

    if (showSession) {

        int total = sessionsTotal;

        if (total < 1) total = 1;
        if (total > 8) total = 8;

        for (int i = 0; i < total; i++) {

            int dotX = 58 - i * 4;

            bool done =
                i < (session - 1);

            bool active =
                i == (session - 1);

            uint16_t c =
                (done || active)
                    ? activeColor
                    : dimColor565(activeColor, 46);

            matrix->drawPixel(
                dotX + offX,
                1 + offY,
                c
            );

            matrix->drawPixel(
                dotX + 1 + offX,
                1 + offY,
                c
            );

            matrix->drawPixel(
                dotX + offX,
                2 + offY,
                c
            );

            matrix->drawPixel(
                dotX + 1 + offX,
                2 + offY,
                c
            );
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public entry point
// ─────────────────────────────────────────────────────────────────────────────

void overdrawPomodoro(
    uint32_t remainingSec,
    uint32_t totalSec,
    uint32_t wallMs,
    uint8_t  phase,
    uint8_t  layout,
    uint8_t  flags,
    uint8_t  session,
    uint8_t  sessionsTotal,
    int8_t   offX,
    int8_t   offY,
    uint16_t activeColor)
{
    if (!(flags & POMO_FLAG_PRESENT)) {
        return;
    }

    const bool isRunning =
        (flags & POMO_FLAG_RUNNING) != 0;

    const bool showSeconds =
        (flags & POMO_FLAG_SECONDS) != 0;

    const bool showSession =
        (flags & POMO_FLAG_SESSION) != 0;

    const bool blinkColor =
        (flags & POMO_FLAG_BLINK) != 0;

    uint32_t liveSec = remainingSec;

    if (isRunning) {

        uint32_t elapsed =
            wallMs / 1000;

        liveSec =
            (elapsed < remainingSec)
                ? (remainingSec - elapsed)
                : 0;
    }

    if (blinkColor && liveSec <= 10) {

        if (((wallMs / 500) % 2) == 1) {
            return;
        }
    }

    bool colonOn =
        ((wallMs % 1000) < 500);

    float progress = 1.0f;

    if (totalSec > 0) {

        progress =
            1.0f -
            ((float)liveSec / (float)totalSec);

        if (progress < 0.0f) progress = 0.0f;
        if (progress > 1.0f) progress = 1.0f;
    }

    switch (layout) {

        case POMO_LAYOUT_MINIMALIST:

            renderMinimalist(
                liveSec,
                progress,
                showSession,
                session,
                sessionsTotal,
                offX,
                offY,
                activeColor
            );

            break;

        case POMO_LAYOUT_SPLIT:
        default:

            renderSplit(
                liveSec,
                progress,
                colonOn,
                showSeconds,
                phase,
                offX,
                offY,
                activeColor
            );

            break;
    }
}