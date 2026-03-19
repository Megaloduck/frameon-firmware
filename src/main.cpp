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

// ── Global preferences ────────────────────────────────────────────────────
static Preferences prefs;

// ── Boot screen ───────────────────────────────────────────────────────────
static void _showBootScreen() {
    matrix->clearScreen();

    // "FRAMEON" in green across the middle
    matrix->setTextSize(1);
    matrix->setTextColor(COL_GREEN);
    matrix->setCursor(4, 4);
    matrix->print("FRAMEON");

    // Horizontal accent line
    matrix->drawFastHLine(0, 13, PANEL_WIDTH, COL_GREEN);

    // Status line
    matrix->setTextColor(rgb(60, 60, 60));
    matrix->setCursor(4, 16);
    matrix->print("Starting...");

    matrix->flushDMABuffer();
}

static void _showWifiScreen(bool connected, const String &ip) {
    matrix->clearScreen();
    matrix->setTextSize(1);

    if (connected) {
        matrix->setTextColor(COL_GREEN);
        matrix->setCursor(2, 2);
        matrix->print("WiFi OK");
        matrix->setTextColor(rgb(80, 80, 80));
        matrix->setCursor(2, 12);
        // Show last two octets of IP to fit 64px
        int lastDot  = ip.lastIndexOf('.');
        int firstDot = ip.indexOf('.');
        matrix->print(ip.substring(firstDot + 1));
        matrix->setCursor(2, 22);
        matrix->print("/update");
    } else {
        matrix->setTextColor(COL_ORANGE);
        matrix->setCursor(2, 2);
        matrix->print("No WiFi");
        matrix->setTextColor(rgb(60, 60, 60));
        matrix->setCursor(2, 12);
        matrix->print("Use app");
        matrix->setCursor(2, 20);
        matrix->print("to setup");
    }

    matrix->flushDMABuffer();
    delay(2500);
}

// ── Setup ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(300); // let CDC settle on S3

    Serial.println("\n[Frameon] Booting...");
    Serial.printf("[Frameon] ESP32-S3 N16R8 | Flash 16MB | PSRAM 8MB\n");

    // ── Matrix ────────────────────────────────────────────────────────
    matrix_init();
    _showBootScreen();

    // ── Preferences ───────────────────────────────────────────────────
    prefs.begin(PREF_NS, false);

    // ── SPIFFS ────────────────────────────────────────────────────────
    if (!SPIFFS.begin(true)) {
        Serial.println("[SPIFFS] Mount failed — formatting...");
        SPIFFS.format();
        SPIFFS.begin(true);
    }
    Serial.printf("[SPIFFS] Total: %u KB, Used: %u KB\n",
        SPIFFS.totalBytes() / 1024, SPIFFS.usedBytes() / 1024);

    // ── Wi-Fi ─────────────────────────────────────────────────────────
    wifi_init(prefs);

    // ── API + OTA server ──────────────────────────────────────────────
    if (wifi_connected()) {
        api_init(prefs);
        clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
    }

    _showWifiScreen(wifi_connected(), wifi_ip());

    Serial.println("[Frameon] Boot complete.");
    Serial.printf("[Frameon] Mode: %d | Brightness: %d\n", g_mode, g_brightness);
}

// ── Loop ──────────────────────────────────────────────────────────────────
void loop() {
    // ── Wi-Fi watchdog + Serial provisioning ──────────────────────────
    bool newConn = wifi_loop(prefs);
    if (newConn) {
        // Just connected (or reconnected) — start API and NTP if needed
        api_init(prefs);
        clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
        Serial.printf("[Frameon] Online. IP: %s\n", wifi_ip().c_str());
    }

    // ── API + OTA loop ─────────────────────────────────────────────────
    if (wifi_connected()) {
        api_loop();
    }

    // ── Display dispatch ───────────────────────────────────────────────
    switch (g_mode) {
        case MODE_CLOCK:
            if (wifi_connected()) clock_draw();
            else {
                // No WiFi — show a static "no time" message
                static uint32_t _noWifiMsg = 0;
                if (millis() - _noWifiMsg > 5000) {
                    _noWifiMsg = millis();
                    matrix->clearScreen();
                    matrix->setTextColor(COL_DIM);
                    matrix->setTextSize(1);
                    matrix->setCursor(4, 12);
                    matrix->print("No WiFi");
                    matrix->flushDMABuffer();
                }
            }
            break;

        case MODE_SPOTIFY:
            spotify_draw();
            break;

        case MODE_GIF:
            // GIF display module — placeholder, implement with AnimatedGIF
            // TODO: gif_draw();
            break;

        case MODE_POMODORO:
            pomodoro_draw();
            break;
    }

    // Small yield to prevent watchdog triggers on S3
    delay(1);
}
