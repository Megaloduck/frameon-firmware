// ─────────────────────────────────────────────────────────────────────────────
// pomodorohelper.cpp — Live Pomodoro timer overdraw renderer (v1.9)
//
// v1.9 — Layout overdraw support.
//        Adds overdrawSplit() and overdrawMinimalist() to match the two new
//        PomodoroLayout options in the Dart app.
//
//        overdrawSplit      — hollow arc ring (r=11, ring width=4) on left
//                             half (cx=14), MM:SS + dim phase label stacked
//                             on right half (centred at x=46).
//
//        overdrawMinimalist — scale-2 minute digits on left (x=2, y=9),
//                             3-px vertical bar on far right (x=61) draining
//                             bottom-up, session dots top-right as 2×2 px
//                             squares, small dim seconds bottom-right.
//
// Both paths share the same liveSec / blink guard computed at the top of
// overdrawPomodoro() so timing behaviour is identical to v1.8.
// ─────────────────────────────────────────────────────────────────────────────

#include "pomodorohelper.h"
#include "frameon.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include <stdio.h>

extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// Polymorph font table (matches clockhelper.cpp and Dart LedFontId.polymorph)
//
// Index mapping: 0-9 → digits, 10 → ':', 11 → ' ' blank
// ─────────────────────────────────────────────────────────────────────────────

struct PomoGlyph { uint8_t w; uint8_t rows[7]; };

static const PomoGlyph kPolymorph[12] = {
    {5, {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, // 0
    {5, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, // 1
    {5, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, // 2
    {5, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}}, // 3
    {5, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, // 4
    {5, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}, // 5
    {5, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, // 6
    {5, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, // 7
    {5, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, // 8
    {5, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}, // 9
    {2, {0x00,0x03,0x03,0x00,0x03,0x03,0x00}}, // : (index 10)
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // ' ' blank (index 11)
};

// Phase label strings — match Dart _phaseLabel()
static const char* kPhaseLabel[3] = { "FOCUS", "BRK", "REST" };

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

static int pomoGlyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    return 11;
}

static int pomoGlyphWidth(char c) {
    return kPolymorph[pomoGlyphIndex(c)].w + 1; // +1 inter-character gap
}

static int pomoTextWidth(const char* s) {
    int w = 0;
    while (*s) w += pomoGlyphWidth(*s++);
    return w > 0 ? w - 1 : 0; // strip trailing gap
}

static void pomoDrawGlyph(char c, int x, int y, uint16_t color) {
    const PomoGlyph& g = kPolymorph[pomoGlyphIndex(c)];
    for (int row = 0; row < 7; row++) {
        uint8_t bits = g.rows[row];
        for (int col = 0; col < (int)g.w; col++) {
            if ((bits >> (g.w - 1 - col)) & 1)
                matrix->drawPixel(x + col, y + row, color);
        }
    }
}

static void pomoDrawText(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        pomoDrawGlyph(*s, x, y, color);
        x += pomoGlyphWidth(*s++);
    }
}

// Scale-2 text: each pixel becomes a 2×2 block.
// Only digits and ':' / ' ' are ever passed here so the glyph table covers
// everything needed.
static void pomoDrawTextScale2(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        const PomoGlyph& g = kPolymorph[pomoGlyphIndex(*s)];
        for (int row = 0; row < 7; row++) {
            uint8_t bits = g.rows[row];
            for (int col = 0; col < (int)g.w; col++) {
                if ((bits >> (g.w - 1 - col)) & 1) {
                    matrix->drawPixel(x + col*2,   y + row*2,   color);
                    matrix->drawPixel(x + col*2+1, y + row*2,   color);
                    matrix->drawPixel(x + col*2,   y + row*2+1, color);
                    matrix->drawPixel(x + col*2+1, y + row*2+1, color);
                }
            }
        }
        x += (g.w + 1) * 2; // +1 gap, scaled
        s++;
    }
}

// Dim a RGB565 colour to `frac` of its brightness (0–256 range, 256=full).
static uint16_t dimColor565(uint16_t c, int frac) {
    int r = ((c >> 11) & 0x1F) * frac >> 8;
    int g = ((c >>  5) & 0x3F) * frac >> 8;
    int b = ((c      ) & 0x1F) * frac >> 8;
    return ((uint16_t)r << 11) | ((uint16_t)g << 5) | (uint16_t)b;
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: POMO_LAYOUT_SPLIT
//
// Left half (x 0–27):  hollow arc ring showing elapsed progress.
//   cx=14, cy=16, outerR=11, innerR=7 — 4-px ring.
//   Arc starts at top (−π/2), sweeps clockwise by progress × 2π.
//   Filled sector uses activeColor; unfilled uses 18% dimmed activeColor.
//
// Right half (x 28–63): MM:SS centred at x=46, top half of right area.
//   Phase label ("FOCUS"/"BRK"/"REST") centred at x=46, below time.
//   Phase label drawn at 35% brightness.
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawSplit(
    uint32_t liveSec,
    float    progress,        // 0.0 – 1.0, elapsed fraction
    bool     colonVisible,
    bool     showSec,
    uint8_t  phase,
    uint16_t activeColor)
{
    const int cx = 14, cy = 16;
    const int outerR = 11, innerR = 7;
    const uint16_t dimArc = dimColor565(activeColor, 46);  // ~18%

    const float startAngle = -(float)M_PI / 2.0f;
    const float sweepEnd   = startAngle + progress * 2.0f * (float)M_PI;

    // Rasterise the ring pixel-by-pixel.
    for (int py = cy - outerR; py <= cy + outerR; py++) {
        for (int px2 = cx - outerR; px2 <= cx + outerR; px2++) {
            float dx2 = (float)(px2 - cx);
            float dy2 = (float)(py - cy);
            float dist = sqrtf(dx2*dx2 + dy2*dy2);
            if (dist < (float)innerR || dist > (float)outerR) continue;

            float angle = atan2f(dy2, dx2);

            // Normalise angle relative to start so we can compare to sweep.
            float norm = angle - startAngle;
            if (norm < 0.0f) norm += 2.0f * (float)M_PI;
            float normEnd = sweepEnd - startAngle;
            if (normEnd < 0.0f) normEnd += 2.0f * (float)M_PI;

            bool filled = (normEnd > 0.001f) && (norm <= normEnd);
            matrix->drawPixel(px2, py, filled ? activeColor : dimArc);
        }
    }

    // MM:SS centred at x=46, upper half of right area.
    char timeBuf[8];
    uint32_t m = liveSec / 60;
    uint32_t s = liveSec % 60;
    if (showSec) {
        snprintf(timeBuf, sizeof(timeBuf), "%02lu%c%02lu",
                 (unsigned long)m, colonVisible ? ':' : ' ', (unsigned long)s);
    } else {
        snprintf(timeBuf, sizeof(timeBuf), "%02lu%c--",
                 (unsigned long)m, colonVisible ? ':' : ' ');
    }

    const int charH = 7;
    const int tw    = pomoTextWidth(timeBuf);
    const int tx    = 46 - tw / 2;
    const int ty    = REAL_HEIGHT / 2 - charH - 1; // row 8 for 32-high panel

    pomoDrawText(timeBuf, tx, ty, activeColor);

    // Phase label at 35% brightness, one lineHeight below time (7+1=8 px).
    const char* label   = (phase < 3) ? kPhaseLabel[phase] : "?";
    const int   lw      = pomoTextWidth(label);
    const int   lx      = 46 - lw / 2;
    const int   ly      = ty + charH + 1;
    const uint16_t dimLabel = dimColor565(activeColor, 90); // ~35%

    pomoDrawText(label, lx, ly, dimLabel);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: POMO_LAYOUT_MINIMALIST
//
// Large minutes (scale-2) at x=2, y=9.
// Vertical bar x=61–63, barTop=3, barBot=28 — drains bottom-up with progress.
//   Filled pixels use activeColor; unfilled use 15% dimmed activeColor.
// Session dots: up to 8 total, 2×2 px, top row (y=0–1), right→left from x=58.
//   Active/done dots use activeColor; future dots 18% dimmed.
// Small seconds (if showSec): dim text bottom-right, just left of bar.
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawMinimalist(
    uint32_t liveSec,
    float    progress,
    bool     showSec,
    bool     showDots,
    uint8_t  session,
    uint8_t  sessionsTotal,
    uint16_t activeColor)
{
    // ── Large minutes ────────────────────────────────────────────────────
    char mBuf[4];
    snprintf(mBuf, sizeof(mBuf), "%02lu", (unsigned long)(liveSec / 60));
    pomoDrawTextScale2(mBuf, 2, 9, activeColor);

    // ── Vertical bar ─────────────────────────────────────────────────────
    const int barX   = 61;
    const int barTop = 3;
    const int barBot = 28;
    const int barH   = barBot - barTop;
    const int filled = (int)(progress * (float)barH + 0.5f);
    const uint16_t dimBar = dimColor565(activeColor, 38); // ~15%

    for (int y = barTop; y < barBot; y++) {
        bool on = (y >= barTop + barH - filled);
        uint16_t c = on ? activeColor : dimBar;
        matrix->drawPixel(barX,     y, c);
        matrix->drawPixel(barX + 1, y, c);
        matrix->drawPixel(barX + 2, y, c);
    }

    // ── Session dots ──────────────────────────────────────────────────────
    if (showDots && sessionsTotal > 0) {
        const int total = (sessionsTotal > 8) ? 8 : (int)sessionsTotal;
        const uint16_t dimDot = dimColor565(activeColor, 46); // ~18%
        for (int i = 0; i < total; i++) {
            int dotX = 58 - i * 4; // right→left
            bool active = (i == (int)(session - 1));
            bool done   = (i <  (int)(session - 1));
            uint16_t dc = (active || done) ? activeColor : dimDot;
            matrix->drawPixel(dotX,     0, dc);
            matrix->drawPixel(dotX + 1, 0, dc);
            matrix->drawPixel(dotX,     1, dc);
            matrix->drawPixel(dotX + 1, 1, dc);
        }
    }

    // ── Small seconds ────────────────────────────────────────────────────
    if (showSec) {
        char sBuf[4];
        snprintf(sBuf, sizeof(sBuf), "%02lu", (unsigned long)(liveSec % 60));
        const int sw  = pomoTextWidth(sBuf);
        const int sx  = barX - sw - 2;
        const int sy  = REAL_HEIGHT - 7 - 1;
        const uint16_t dimSec = dimColor565(activeColor, 128); // ~50%
        pomoDrawText(sBuf, sx, sy, dimSec);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawPomodoro — v1.9  (public entry point)
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
    if (!(flags & POMO_FLAG_PRESENT)) return;

    const bool isRunning  = (flags & POMO_FLAG_RUNNING) != 0;
    const bool showSec    = (flags & POMO_FLAG_SECONDS) != 0;
    const bool showDots   = (flags & POMO_FLAG_SESSION) != 0;
    const bool blinkColon = (flags & POMO_FLAG_BLINK)   != 0;

    // ── Live remaining seconds ────────────────────────────────────────────
    uint32_t liveSec = remainingSec;
    if (isRunning) {
        const uint32_t elapsed = wallMs / 1000;
        liveSec = (elapsed < remainingSec) ? (remainingSec - elapsed) : 0;
    }

    // ── 10-second warning blink ───────────────────────────────────────────
    if (liveSec <= 10 && ((wallMs / 500) % 2 == 1)) return;

    // ── Colon visibility ──────────────────────────────────────────────────
    const bool colonVisible = !blinkColon || ((wallMs % 1000) < 500);

    // ── Elapsed progress fraction ─────────────────────────────────────────
    float progress = 0.0f;
    if (totalSec > 0) {
        uint32_t elapsed = (liveSec < totalSec) ? (totalSec - liveSec) : totalSec;
        progress = (float)elapsed / (float)totalSec;
        if (progress > 1.0f) progress = 1.0f;
    }

    // offX / offY nudges — not applied to bar/arc but to text origin in
    // the split layout. For minimalist, the whole composition shifts.
    // (Kept simple: only text origins shift, structural elements are fixed.)
    (void)offX; (void)offY; // reserved for future use

    // ── Dispatch to layout renderer ───────────────────────────────────────
    if (layout == POMO_LAYOUT_MINIMALIST) {
        overdrawMinimalist(liveSec, progress, showSec, showDots,
                           session, sessionsTotal, activeColor);
    } else {
        // POMO_LAYOUT_SPLIT (default — also catches any unknown value)
        overdrawSplit(liveSec, progress, colonVisible, showSec,
                      phase, activeColor);
    }
}