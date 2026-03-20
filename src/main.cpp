#include <Arduino.h>
#include <Preferences.h>
#include <SPIFFS.h>
#include <WiFi.h>
#include <time.h>

#include "config.h"
#include "matrix.h"
#include "wifi_manager.h"
#include "api_server.h"
#include "usb_api.h"
#include "display_clock.h"
#include "display_pomodoro.h"
#include "display_spotify.h"

static Preferences prefs;
static bool        _clockInited = false; // guard: only init clock once per WiFi session

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

void setup() {
    // FIX: increase USB CDC serial receive buffer from 256 to 4096 bytes.
    // A single clock_config command with a long POSIX timezone string can
    // approach the 256-byte default limit. Larger buffer prevents data loss
    // when commands arrive faster than usb_api_loop() can drain them.
    Serial.setRxBufferSize(4096);
    Serial.begin(SERIAL_BAUD);
    delay(300);

    Serial.println("\n[Frameon] Booting...");
    Serial.println("[Frameon] ESP32-S3 N16R8 | 16MB Flash | 8MB PSRAM");
    Serial.printf("[Frameon] Virtual canvas %dx%d — physical %dx%d\n",
                  PANEL_WIDTH, PANEL_HEIGHT, PANEL_WIDTH, REAL_HEIGHT);

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

    // USB API init — must be before WiFi so commands work without a network.
    usb_api_init(prefs);

    wifi_init(prefs);

    if (wifi_connected()) {
        api_init(prefs);
        // clock_init() deferred _ntp->update() so it won't block here.
        clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
        _clockInited = true;
    }

    _showWifiScreen(wifi_connected(), wifi_ip());

    String statusJson = "{\"connected\":" + String(wifi_connected() ? "true" : "false") +
                        ",\"ip\":\"" + wifi_ip() + "\"" +
                        ",\"mode\":" + String((int)g_mode) +
                        ",\"brightness\":" + String(g_brightness) + "}";
    usb_push("boot", statusJson);

    Serial.println("[Frameon] Boot complete.");
    Serial.printf("[Frameon] Mode: %d | Brightness: %d\n", g_mode, g_brightness);
}

void loop() {
    // ── USB API — highest priority ────────────────────────────────────────
    // Must run every iteration without being blocked by anything below it.
    // Any blocking call (NTP sync, NVS write, WiFi connect) goes AFTER this.
    usb_api_loop(prefs);

    // ── WiFi reconnect ────────────────────────────────────────────────────
    bool newConn = wifi_loop(prefs);
    if (newConn) {
        api_init(prefs);
        // Only init clock once per WiFi session — calling clock_init() again
        // on every reconnect causes unnecessary NTP re-init and potential delays.
        if (!_clockInited) {
            clock_init(prefs.getString(PREF_NTP_SERVER, NTP_DEFAULT_SERVER).c_str());
            _clockInited = true;
        }
        Serial.printf("[Frameon] Online. IP: %s\n", wifi_ip().c_str());
        usb_push("wifi_connected", "{\"ip\":\"" + wifi_ip() + "\"}");
    }

    // Reset clock init flag if WiFi drops, so it re-inits on next connect
    if (!wifi_connected() && _clockInited) {
        _clockInited = false;
    }

    if (wifi_connected()) {
        api_loop();
    }

    // ── Display ───────────────────────────────────────────────────────────
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
        case MODE_SPOTIFY:  spotify_draw();  break;
        case MODE_GIF:      /* TODO: gif_draw(); */ break;
        case MODE_POMODORO: pomodoro_draw(); break;
    }

    delay(1);
}
