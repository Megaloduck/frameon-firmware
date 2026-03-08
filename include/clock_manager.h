#pragma once

#include <Arduino.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════
//  clock_manager.h
//
//  Owns the software RTC (synced from phone via BLE) and the clock-face
//  rendering task.  Uses the ESP32's built-in timeval / gettimeofday for
//  timekeeping — no external RTC chip required, though one can be added.
// ═══════════════════════════════════════════════════════════════════════════

class ClockManager {
public:
    // ── Configuration (set by BLE ClockConfig write) ──────────────────────────
    struct Config {
        uint32_t epochUtc  = 0;     // Unix timestamp at sync moment
        bool     is24h         = true;
        bool     showSeconds   = false;
        bool     showDate      = false;
    };

    ClockManager();

    // Sync the internal RTC with a Unix epoch received from the phone.
    void syncTime(const Config& cfg);

    // Returns current config.
    const Config& config() const { return _cfg; }

    // Fill a 64×32 RGB565 frame with the current clock face.
    // Called from the clock task every CLOCK_REFRESH_MS.
    void renderToBuffer(uint8_t* rgb565Out);

    // Start the FreeRTOS task that periodically renders the clock.
    void startTask();
    void stopTask();

    bool isRunning() const { return _taskHandle != nullptr; }

private:
    Config             _cfg;
    TaskHandle_t       _taskHandle = nullptr;
    volatile bool      _stop       = false;

    // ── Rendering helpers ─────────────────────────────────────────────────────
    void _drawDigits(uint8_t* buf, int x, int y,
                     const char* str, uint16_t color565, int scale);
    void _drawChar  (uint8_t* buf, int x, int y,
                     char c,      uint16_t color565, int scale);
    void _drawSeparator(uint8_t* buf, int x, int y,
                        uint16_t color565, bool visible);

    // Write one RGB565 pixel into the flat buffer (big-endian).
    static void _setPixel(uint8_t* buf, int col, int row, uint16_t color565);

    static void _taskEntry(void* param);
    void        _taskLoop();

    // ── Tiny 3×5 pixel font for clock digits (0-9, colon, slash, space) ───────
    // Each char is 5 bytes: one per row, MSB = leftmost pixel, 3 bits wide.
    static const uint8_t _font3x5[][5];
    static int           _charIndex(char c);
};

ClockManager& Clock();