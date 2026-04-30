// ─────────────────────────────────────────────────────────────────────────────
// pomodorohelper.cpp — Live Pomodoro timer overdraw renderer (v1.10)
//
// v1.10 — Fixed phase label rendering in splitLayout.
//         kPolymorph expanded from 12 (digits + ':' + space) to 22 entries,
//         adding the 10 uppercase letters needed for the phase labels:
//           B C E F K O R S T U  →  "FOCUS", "BRK", "REST"
//         pomoGlyphIndex() updated to map these letters to the new slots.
//         Bitmaps copied verbatim from lib/engine/renderer/fonts/polymorph_font.dart
//         so the panel matches the app preview pixel-for-pixel.
//
// v1.9 — Layout overdraw support (splitLayout / minimalist).
// v1.8 — Initial Pomodoro live overdraw via millis().
// ─────────────────────────────────────────────────────────────────────────────

#include "pomodorohelper.h"
#include "frameon.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <math.h>
#include <stdio.h>

extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// Polymorph glyph table
//
// Index mapping:
//   0-9  → '0'-'9'
//   10   → ':'
//   11   → ' '  (blank / fallback)
//   12   → 'B'
//   13   → 'C'
//   14   → 'E'
//   15   → 'F'
//   16   → 'K'
//   17   → 'O'
//   18   → 'R'
//   19   → 'S'
//   20   → 'T'
//   21   → 'U'
//
// Bitmaps from lib/engine/renderer/fonts/polymorph_font.dart.
// Each row is a bitmask — MSB = leftmost pixel, w bits wide.
// ─────────────────────────────────────────────────────────────────────────────

struct PomoGlyph { uint8_t w; uint8_t rows[7]; };

static const PomoGlyph kPolymorph[23] = {
    // ── Digits ───────────────────────────────────────────────────────────
    {5, {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}}, //  0 → '0'
    {5, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}}, //  1 → '1'
    {5, {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}}, //  2 → '2'
    {5, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}}, //  3 → '3'
    {5, {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}}, //  4 → '4'
    {5, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}}, //  5 → '5'
    {5, {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}}, //  6 → '6'
    {5, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}}, //  7 → '7'
    {5, {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}}, //  8 → '8'
    {5, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}}, //  9 → '9'
    // ── Punctuation ──────────────────────────────────────────────────────
    {2, {0x00,0x03,0x03,0x00,0x03,0x03,0x00}}, // 10 → ':'
    {2, {0x00,0x00,0x00,0x00,0x00,0x00,0x00}}, // 11 → ' ' blank / fallback
    // ── Letters for phase labels (FOCUS, BREAK, REST) ───────────────────────
    {5, {0x1E,0x09,0x09,0x0E,0x09,0x09,0x1E}}, // 12 → 'B'
    {5, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}}, // 13 → 'C'
    {5, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}}, // 14 → 'E'
    {5, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}}, // 15 → 'F'   
    {5, {0x11,0x12,0x14,0x18,0x14,0x12,0x11}}, // 16 → 'K'
    {5, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}}, // 17 → 'O'
    {5, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}}, // 18 → 'R'
    {5, {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E}}, // 19 → 'S'
    {5, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}}, // 20 → 'T'
    {5, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}}, // 21 → 'U'
    {5, {0x04,0x0A,0x11,0x11,0x1F,0x11,0x11}} // 22→ 'A'
};

// Phase label strings — must match Dart _phaseLabel() in pomodoro_widget.dart
static const char* kPhaseLabel[3] = { "FOCUS", "BREAK", "REST" };

// ─────────────────────────────────────────────────────────────────────────────
// Glyph index lookup
// ─────────────────────────────────────────────────────────────────────────────

static int pomoGlyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':')  return 10;
    // Uppercase letters needed for phase labels
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
        case 'A':  return 22; 
        default:  return 11; // blank fallback
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

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
// Only digits and ':' / ' ' are ever passed here.
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

// Dim a RGB565 colour to `frac/256` of its brightness (0=black, 256=full).
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
// Right half (x 28–63): MM:SS centred at x=46, upper half of right area.
//   Phase label ("FOCUS"/"BREAK"/"REST") centred at x=46, one lineHeight below.
//   Phase label drawn at 35% brightness.
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawSplit(
    uint32_t liveSec,
    float    progress,
    bool     colonVisible,
    bool     showSec,
    uint8_t  phase,
    uint16_t activeColor)
{
    const int cx = 14, cy = 16;
    const int outerR = 11, innerR = 7;
    const uint16_t dimArc = dimColor565(activeColor, 46); // ~18%

    const float startAngle = -(float)M_PI / 2.0f;
    const float sweepEnd   = startAngle + progress * 2.0f * (float)M_PI;

    // ── Arc ring ─────────────────────────────────────────────────────────
    for (int py = cy - outerR; py <= cy + outerR; py++) {
        for (int px2 = cx - outerR; px2 <= cx + outerR; px2++) {
            float dx2  = (float)(px2 - cx);
            float dy2  = (float)(py  - cy);
            float dist = sqrtf(dx2*dx2 + dy2*dy2);
            if (dist < (float)innerR || dist > (float)outerR) continue;

            float angle   = atan2f(dy2, dx2);
            float norm    = angle - startAngle;
            if (norm < 0.0f) norm += 2.0f * (float)M_PI;
            float normEnd = sweepEnd - startAngle;
            if (normEnd < 0.0f) normEnd += 2.0f * (float)M_PI;

            bool filled = (normEnd > 0.001f) && (norm <= normEnd);
            matrix->drawPixel(px2, py, filled ? activeColor : dimArc);
        }
    }

    // ── MM:SS (right half, centred at x=46) ──────────────────────────────
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
    const int ty    = REAL_HEIGHT / 2 - charH - 1; // row 8 for 32-px-tall panel

    pomoDrawText(timeBuf, tx, ty, activeColor);

    // ── Phase label (full brightness, one lineHeight below time) ──────────
    // kPhaseLabel uses only letters in our extended table (B,C,E,F,K,O,R,S,T,U,A)
    // so pomoGlyphIndex() now resolves them correctly.
    const char* label = (phase < 3) ? kPhaseLabel[phase] : "?";
    const int   lw    = pomoTextWidth(label);
    const int   lx    = 46 - lw / 2;
    const int   ly    = ty + charH + 1; // one lineHeight (7+1=8 px) below time

    pomoDrawText(label, lx, ly, activeColor);
}

// ─────────────────────────────────────────────────────────────────────────────
// Layout: POMO_LAYOUT_MINIMALIST
//
// Small MM label   — normal scale at (2, 1), always visible.
// Large SS         — scale-2 at (2, 9), behind POMO_FLAG_SECONDS.
// Vertical bar     — 2 px wide at x=62..63, y=1..30, drains bottom-up.
//                    Filled = activeColor; unfilled = 15% dim.
// Session dots     — 2×2 px, y=1..2, right→left from x=58, if showDots.
//                    Active/done = activeColor; future = 18% dim.
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
    // ── Small minutes label (top-left, above big seconds) ────────────────
    char mBuf[4];
    snprintf(mBuf, sizeof(mBuf), "%02lu", (unsigned long)(liveSec / 60));
    pomoDrawText(mBuf, 2, 1, activeColor);

    // ── Large seconds (scale-2, at y=9) ──────────────────────────────────
    if (showSec) {
        char sBuf[4];
        snprintf(sBuf, sizeof(sBuf), "%02lu", (unsigned long)(liveSec % 60));
        pomoDrawTextScale2(sBuf, 2, 9, activeColor);
    }

    // ── Vertical bar (x=62-63, 1px margins all sides) ────────────────────
    const int barX   = 62;
    const int barTop = 1;
    const int barBot = 31;
    const int barH   = barBot - barTop;
    const int filled = (int)(progress * (float)barH + 0.5f);
    const uint16_t dimBar = dimColor565(activeColor, 38); // ~15%

    for (int y = barTop; y < barBot; y++) {
        bool     on = (y >= barTop + barH - filled);
        uint16_t c  = on ? activeColor : dimBar;
        matrix->drawPixel(barX,     y, c);
        matrix->drawPixel(barX + 1, y, c);
    }

    // ── Session dots (y=1-2, 1px top margin, right→left from x=58) ───────
    if (showDots && sessionsTotal > 0) {
        const int      total  = (sessionsTotal > 8) ? 8 : (int)sessionsTotal;
        const uint16_t dimDot = dimColor565(activeColor, 46); // ~18%
        for (int i = 0; i < total; i++) {
            int      dotX   = 58 - i * 4;
            bool     active = (i == (int)(session - 1));
            bool     done   = (i <  (int)(session - 1));
            uint16_t dc     = (active || done) ? activeColor : dimDot;
            matrix->drawPixel(dotX,     1, dc);
            matrix->drawPixel(dotX + 1, 1, dc);
            matrix->drawPixel(dotX,     2, dc);
            matrix->drawPixel(dotX + 1, 2, dc);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawPomodoro — v1.10  (public entry point)
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

    (void)offX; (void)offY; // reserved for future use

    // ── Dispatch to layout renderer ───────────────────────────────────────
    if (layout == POMO_LAYOUT_MINIMALIST) {
        overdrawMinimalist(liveSec, progress, showSec, showDots,
                           session, sessionsTotal, activeColor);
    } else {
        // POMO_LAYOUT_SPLIT (default — also handles any unknown value)
        overdrawSplit(liveSec, progress, colonVisible, showSec,
                      phase, activeColor);
    }
}