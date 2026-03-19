#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "matrix.h"
#include "wifi_manager.h"
#include "api_server.h"
#include "display_clock.h"
#include "display_pomodoro.h"
#include "display_spotify.h"

static Preferences prefs;
static uint32_t _lastScroll   = 0;
static int      _scrollOffset = 0;
static String   _lastTrack    = "";

static void _showBootScreen() {
    matrix->clearScreen();
    matrix->setTextSize(1);
    matrix->setTextColor(COL_GREEN());
    matrix->setCursor(4, 4);
    matrix->print("FRAMEON");
    matrix->drawFastHLine(0, 13, PANEL_WIDTH, COL_GREEN());
    matrix->setTextColor(rgb(60, 60, 60));
    matrix->setCursor(4, 16);
    matrix->print("Starting...");
    matrix->flipDMABuffer();
}

static void _drawScrollingText(const String &line1, const String &line2) {
    if (line1 != _lastTrack) {
        _scrollOffset = 0;
        _lastTrack    = line1;
    }

    if (millis() - _lastScroll > 80) {
        _lastScroll = millis();
        // Right half is 32 px wide; scroll if text wider than that
        int textWidth = line1.length() * 6;
        if (textWidth > 32) {
            _scrollOffset++;
            if (_scrollOffset > textWidth) _scrollOffset = 0;
        }
    }

    // Track name — scrolling, right half starting at x=32
    matrix->setTextSize(1);
    matrix->setTextColor(COL_WHITE());
    matrix->setCursor(32 - _scrollOffset, 2);   // y=2, bottom at y=9 ✓
    matrix->print(line1);

    // Artist name — truncate to 5 chars to fit right half
    matrix->setTextColor(rgb(150, 150, 150));
    matrix->setCursor(32, 12);                   // y=12, bottom at y=19 ✓
    String artist = line2;
    if (artist.length() > 5) artist = artist.substring(0, 5);
    matrix->print(artist);

    // Play/pause indicator — right half, bottom area
    // y=22, height=8 → bottom at y=29 ✓ (within REAL_HEIGHT=32)
    uint16_t playColor = g_spotify.isPlaying ? COL_SPOTIFY() : COL_DIM();
    matrix->fillRect(32, 22, 10, 8, COL_BLACK());
    if (g_spotify.isPlaying) {
        // Play triangle (filled approximation)
        for (int i = 0; i < 4; i++) {
            matrix->drawFastVLine(32 + i, 22 + i, 8 - i * 2, playColor);
        }
    } else {
        // Pause bars
        matrix->drawFastVLine(32, 22, 8, playColor);
        matrix->drawFastVLine(35, 22, 8, playColor);
    }
}

static void _drawArtPlaceholder() {
    // Album art occupies the left 32×REAL_HEIGHT area.
    // Use REAL_HEIGHT (32) — NOT the virtual PANEL_HEIGHT (64).
    matrix->fillRect(0, 0, 32, REAL_HEIGHT, rgb(10, 20, 15));
    matrix->drawRect(0, 0, 32, REAL_HEIGHT, COL_SPOTIFY());

    matrix->setTextColor(COL_SPOTIFY());
    matrix->setTextSize(2);
    matrix->setCursor(8, 8);   // 16px tall text → bottom at y=23 ✓
    matrix->print("S");
}

static void _showWifiScreen(bool connected, const String &ip) {
    matrix->clearScreen();
    matrix->setTextSize(1);

    if (connected) {
        matrix->setTextColor(COL_GREEN());
        matrix->setCursor(2, 2);
        matrix->print("WiFi OK");
        matrix->setTextColor(rgb(80, 80, 80));
        matrix->setCursor(2, 12);
        int firstDot = ip.indexOf('.');
        matrix->print(ip.substring(firstDot + 1));
        matrix->setCursor(2, 22);
        matrix->print("/update");
    } else {
        matrix->setTextColor(COL_ORANGE());
        matrix->setCursor(2, 2);
        matrix->print("No WiFi");
        matrix->setTextColor(rgb(60, 60, 60));
        matrix->setCursor(2, 12);
        matrix->print("Use app");
        matrix->setCursor(2, 20);
        matrix->print("to setup");
    }

    matrix->flipDMABuffer();
    delay(2500);
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

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(300);

    Serial.println("\n[Frameon] Booting...");
    Serial.println("[Frameon] ESP32-S3 N16R8 | 16MB Flash | 8MB PSRAM");

    matrix_init();
    _showBootScreen();

    prefs.begin(PREF_NS, false);

    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Mount failed — formatting...");
        SPIFFS.format();
        SPIFFS.begin(true);
    }
    Serial.printf("[SPIFFS] %u KB total, %u KB used\n",
        SPIFFS.totalBytes() / 1024, SPIFFS.usedBytes() / 1024);

    wifi_init(prefs);

    if (wifi_connected()) {
        api_init(prefs);
        clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
    }

    _showWifiScreen(wifi_connected(), wifi_ip());

    Serial.println("[Frameon] Boot complete.");
    Serial.printf("[Frameon] Mode: %d | Brightness: %d\n", g_mode, g_brightness);
}

void loop() {
    bool newConn = wifi_loop(prefs);
    if (newConn) {
        api_init(prefs);  // safe — internally guarded against double-init
        clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
        Serial.printf("[Frameon] Online. IP: %s\n", wifi_ip().c_str());
    }

    if (wifi_connected()) {
        api_loop();
    }

    switch (g_mode) {
        case MODE_CLOCK:
            if (wifi_connected()) {
                clock_draw();
            } else {
                static uint32_t _noWifiMsg = 0;
                if (millis() - _noWifiMsg > 5000) {
                    _noWifiMsg = millis();
                    matrix->clearScreen();
                    matrix->setTextColor(COL_DIM());
                    matrix->setTextSize(1);
                    matrix->setCursor(4, 12);
                    matrix->print("No WiFi");
                    matrix->flipDMABuffer();
                }
            }
            break;

        case MODE_SPOTIFY:
            spotify_draw();
            break;

        case MODE_GIF:
            // TODO: gif_draw();
            break;

        case MODE_POMODORO:
            pomodoro_draw();
            break;
    }

    delay(1);
}