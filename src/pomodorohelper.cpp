// ─────────────────────────────────────────────────────────────────────────────
// pomodorohelper.cpp — Live Pomodoro timer overdraw renderer (v1.8)
//
// Draws the countdown timer on top of the current frame using the same
// Polymorph font as the clock and the same millis()-based approach as
// overdrawClock() so seconds always tick correctly on the panel,
// independent of the animation loop length.
//
// The Pomodoro always uses the Polymorph font (fontId 0) because that is
// the only font the Dart PomodoroWidget uses (LedFontId.polymorph), so the
// panel always matches the app preview pixel-for-pixel.
//
// Rendering is intentionally minimal:
//   • MM:SS centered on the panel (with X/Y offset)
//   • Blink-colon at 1 Hz when POMO_FLAG_BLINK is set
//   • 10-second warning: entire display blinks every 500 ms
//   • Session dots in the bottom-right corner when POMO_FLAG_SESSION is set
//   • Timer freezes (no decrement) when POMO_FLAG_RUNNING is clear
// ─────────────────────────────────────────────────────────────────────────────

#include "pomodorohelper.h"
#include "frameon.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <stdio.h>

extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// Polymorph font — copied from clockhelper.cpp (single source matches Dart)
//
// Index mapping:
//   0-9 → '0'-'9'   10 → ':'   11 → ' ' (space, used as blank colon)
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

// ─────────────────────────────────────────────────────────────────────────────
// Font helpers
// ─────────────────────────────────────────────────────────────────────────────

static int pomoGlyphIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    return 11; // space / unknown → blank
}

static int pomoGlyphWidth(char c) {
    return kPolymorph[pomoGlyphIndex(c)].w + 1; // +1 inter-character gap
}

static int pomoTextWidth(const char* s) {
    int w = 0;
    while (*s) { w += pomoGlyphWidth(*s++); }
    return w > 0 ? w - 1 : 0; // no trailing gap
}

static void pomoDrawGlyph(char c, int x, int y, uint16_t color) {
    const PomoGlyph& g = kPolymorph[pomoGlyphIndex(c)];
    for (int row = 0; row < 7; row++) {
        const uint8_t bits = g.rows[row];
        for (int col = 0; col < (int)g.w; col++) {
            if ((bits >> (g.w - 1 - col)) & 1) {
                matrix->drawPixel(x + col, y + row, color);
            }
        }
    }
}

static void pomoDrawText(const char* s, int x, int y, uint16_t color) {
    while (*s) {
        pomoDrawGlyph(*s, x, y, color);
        x += pomoGlyphWidth(*s);
        s++;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawPomodoro — v1.8
// ─────────────────────────────────────────────────────────────────────────────

void overdrawPomodoro(
    uint32_t remainingSec,
    uint32_t wallMs,
    uint8_t  phase,
    uint8_t  flags,
    uint8_t  session,
    int8_t   offX,
    int8_t   offY,
    uint16_t activeColor)
{
    // Guard: only render if present flag is set.
    if (!(flags & POMO_FLAG_PRESENT)) return;

    const bool isRunning  = (flags & POMO_FLAG_RUNNING)  != 0;
    const bool showSec    = (flags & POMO_FLAG_SECONDS)   != 0;
    const bool showDots   = (flags & POMO_FLAG_SESSION)   != 0;
    const bool blinkColon = (flags & POMO_FLAG_BLINK)     != 0;

    // ── Compute live remaining seconds ────────────────────────────────────
    // Only subtract elapsed time when the timer was running at commit.
    // wallMs wraps after ~49 days — safe for any realistic session length.
    uint32_t liveSec = remainingSec;
    if (isRunning) {
        const uint32_t elapsedSec = wallMs / 1000;
        liveSec = (elapsedSec < remainingSec) ? (remainingSec - elapsedSec) : 0;
    }

    // ── 10-second warning blink ───────────────────────────────────────────
    // Match the Dart renderWithState() behaviour: when ≤10 s remain,
    // skip drawing entirely every other 500 ms slice.
    if (liveSec <= 10 && ((wallMs / 500) % 2 == 1)) return;

    // ── Colon visibility ──────────────────────────────────────────────────
    // When blinkColon is on the colon alternates at 1 Hz using wallMs.
    // When blinkColon is off the colon is always solid.
    const bool colonVisible = !blinkColon || ((wallMs % 1000) < 500);

    // ── Format time string ────────────────────────────────────────────────
    const uint32_t m = liveSec / 60;
    const uint32_t s = liveSec % 60;

    char timeBuf[8]; // "MM:SS\0" or "MM \0"
    if (showSec) {
        snprintf(timeBuf, sizeof(timeBuf), "%02lu:%02lu",
                 (unsigned long)m, (unsigned long)s);
    } else {
        // No seconds: show "MM:--" with a fixed-width placeholder so the
        // display width stays constant (matches Dart _format behaviour).
        snprintf(timeBuf, sizeof(timeBuf), "%02lu:--", (unsigned long)m);
    }

    // Replace colon character with a space glyph when colon is hidden
    // (space glyph is all-zero pixels but has the same width as ':').
    if (!colonVisible) {
        for (int i = 0; timeBuf[i]; i++) {
            if (timeBuf[i] == ':') { timeBuf[i] = ' '; break; }
        }
    }

    // ── Layout: centre the text on the panel ──────────────────────────────
    const int charH = 7;
    const int textW = pomoTextWidth(timeBuf);
    const int x     = (PANEL_WIDTH  - textW) / 2 + (int)offX;
    const int y     = (REAL_HEIGHT  - charH) / 2 + (int)offY;

    pomoDrawText(timeBuf, x, y, activeColor);

    // ── Session dots ──────────────────────────────────────────────────────
    // Small 2×2 px squares in the bottom-right corner, one per session.
    // Matches the Dart _renderSessionDots() layout exactly.
    if (showDots && session > 0) {
        const int ds = 2, dg = 1;
        int dx = PANEL_WIDTH - (session * ds + (session - 1) * dg) - 2;
        const int dy = REAL_HEIGHT - ds - 1;
        for (int i = 0; i < (int)session; i++) {
            for (int ry = dy; ry < dy + ds; ry++) {
                for (int rx = dx; rx < dx + ds; rx++) {
                    matrix->drawPixel(rx, ry, activeColor);
                }
            }
            dx += ds + dg;
        }
    }

    // NOTE: flipDMABuffer() is called by displayTask after this returns.
}