#include "display_spotify.h"
#include "matrix.h"
#include "api_server.h"

static uint32_t _lastScroll   = 0;
static int      _scrollOffset = 0;
static String   _lastTrack    = "";

// ── Scrolling text on right panel half ────────────────────────────────────
static void _drawScrollingText(const String &line1, const String &line2) {
    // Each character in default font = 6px wide
    // Right panel half: x=32..63 (32px wide)

    // Reset scroll when track changes
    if (line1 != _lastTrack) {
        _scrollOffset = 0;
        _lastTrack    = line1;
    }

    // Scroll every 80ms
    if (millis() - _lastScroll > 80) {
        _lastScroll = millis();
        int textWidth = line1.length() * 6;
        if (textWidth > 32) {
            _scrollOffset++;
            if (_scrollOffset > textWidth) _scrollOffset = 0;
        }
    }

    // Draw track name (scrolling if too wide)
    matrix->setTextSize(1);
    matrix->setTextColor(COL_WHITE);
    matrix->setCursor(32 - _scrollOffset, 2);
    matrix->print(line1);

    // Artist (static, truncated)
    matrix->setTextColor(rgb(150, 150, 150));
    matrix->setCursor(32, 12);
    String artist = line2;
    if (artist.length() > 5) artist = artist.substring(0, 5);
    matrix->print(artist);

    // Playing indicator
    uint16_t playColor = g_spotify.isPlaying ? COL_SPOTIFY : COL_DIM;
    matrix->fillRect(32, 22, 4, 8, COL_DIM);
    if (g_spotify.isPlaying) {
        // Draw simple play triangle
        for (int i = 0; i < 4; i++) {
            matrix->drawFastVLine(32 + i, 22 + i, 8 - i * 2, playColor);
        }
    } else {
        // Pause bars
        matrix->drawFastVLine(32, 22, 8, playColor);
        matrix->drawFastVLine(35, 22, 8, playColor);
    }
}

// ── Album art (left 32×32, JPEG decoded by Flutter → base64 → raw RGB) ──
// For now draws a coloured placeholder; full JPEG decode in next iteration.
static void _drawArtPlaceholder() {
    // Solid accent rectangle with "♪" hint
    matrix->fillRect(0, 0, 32, 32, rgb(10, 20, 15));
    matrix->drawRect(0, 0, 32, 32, COL_SPOTIFY);
    matrix->setTextColor(COL_SPOTIFY);
    matrix->setTextSize(2);
    matrix->setCursor(8, 8);
    matrix->print("S");
}

void spotify_draw() {
    static uint32_t _lastDraw = 0;
    if (millis() - _lastDraw < 40) return; // ~25fps cap
    _lastDraw = millis();

    matrix->clearScreen();
    _drawArtPlaceholder();
    _drawScrollingText(g_spotify.track, g_spotify.artist);
    matrix->flushDMABuffer();
}
