#include "display_spotify.h"
#include "matrix.h"
#include "api_server.h"

static uint32_t _lastScroll   = 0;
static int      _scrollOffset = 0;
static String   _lastTrack    = "";

static void _drawScrollingText(const String &line1, const String &line2) {
    if (line1 != _lastTrack) {
        _scrollOffset = 0;
        _lastTrack    = line1;
    }

    if (millis() - _lastScroll > 80) {
        _lastScroll = millis();
        int textWidth = line1.length() * 6;
        if (textWidth > 32) {
            _scrollOffset++;
            if (_scrollOffset > textWidth) _scrollOffset = 0;
        }
    }

    matrix->setTextSize(1);
    matrix->setTextColor(COL_WHITE());
    matrix->setCursor(32 - _scrollOffset, 2);
    matrix->print(line1);

    matrix->setTextColor(rgb(150, 150, 150));
    matrix->setCursor(32, 12);
    String artist = line2;
    if (artist.length() > 5) artist = artist.substring(0, 5);
    matrix->print(artist);

    uint16_t playColor = g_spotify.isPlaying ? COL_SPOTIFY() : COL_DIM();
    matrix->fillRect(32, 22, 10, 8, COL_BLACK());
    if (g_spotify.isPlaying) {
        for (int i = 0; i < 4; i++) {
            matrix->drawFastVLine(32 + i, 22 + i, 8 - i * 2, playColor);
        }
    } else {
        matrix->drawFastVLine(32, 22, 8, playColor);
        matrix->drawFastVLine(35, 22, 8, playColor);
    }
}

static void _drawArtPlaceholder() {
    matrix->fillRect(0, 0, 32, 32, rgb(10, 20, 15));
    matrix->drawRect(0, 0, 32, 32, COL_SPOTIFY());
    matrix->setTextColor(COL_SPOTIFY());
    matrix->setTextSize(2);
    matrix->setCursor(8, 8);
    matrix->print("S");
}

void spotify_draw() {
    static uint32_t _lastDraw = 0;
    if (millis() - _lastDraw < 40) return;
    _lastDraw = millis();

    matrix->clearScreen();
    _drawArtPlaceholder();
    _drawScrollingText(g_spotify.track, g_spotify.artist);
    matrix->flipDMABuffer();
}
