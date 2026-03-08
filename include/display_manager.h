#pragma once

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h> // HUB75 DMA driver for ESP32-S3
#include <NimBLEDevice.h>                 // NimBLE BLE stack (much lighter than built-in BLE library)
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════
//  display_manager.h
//
//  Single owner of the HUB75 DMA panel.  All other modules call into this
//  instead of touching the panel directly.
//
//  Thread-safety: every public method that touches the panel acquires
//  _mutex, so it is safe to call from both the BLE task and the clock task.
// ═══════════════════════════════════════════════════════════════════════════

// ── GIF frame descriptor ─────────────────────────────────────────────────────

struct GifFrame {
    uint8_t* pixels;    // FRAME_BYTES of RGB565 (big-endian)
    uint16_t durationMs;
};

// ── Display manager ───────────────────────────────────────────────────────────

class DisplayManager {
public:
    DisplayManager();

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool begin();                       // call once in setup()
    void setBrightness(uint8_t value);  // 0–255 → mapped to panel 0–100

    // ── Still frame ───────────────────────────────────────────────────────────
    // Push one full FRAME_BYTES buffer of big-endian RGB565 to the panel.
    void showFrame(const uint8_t* rgb565, size_t len);

    // ── GIF playback ─────────────────────────────────────────────────────────
    // Store a decoded GIF frame into the internal ring buffer.
    // Returns false if the frame index is out of range.
    bool storeGifFrame(uint8_t index, const uint8_t* rgb565,
                       size_t len, uint16_t durationMs);
    void setGifFrameCount(uint8_t count);
    void startGifPlayback();   // kicks off the GIF playback loop
    void stopGifPlayback();

    // ── Clock mode ────────────────────────────────────────────────────────────
    // Render a simple HH:MM (or HH:MM:SS) clock face directly onto the panel.
    void renderClock(bool is24h, bool showSeconds, bool showDate);

    // ── Utilities ─────────────────────────────────────────────────────────────
    void clear();
    MatrixPanel_I2S_DMA* panel() { return _panel; }

    // Runs inside a FreeRTOS task — call startTask() to create it.
    void startTask();

private:
    MatrixPanel_I2S_DMA* _panel = nullptr;
    SemaphoreHandle_t     _mutex;
    uint8_t               _brightness = DEFAULT_BRIGHTNESS;

    // ── GIF state ─────────────────────────────────────────────────────────────
    GifFrame  _gifFrames[GIF_MAX_FRAMES];
    uint8_t   _gifFrameCount  = 0;
    uint8_t   _gifCurrentIdx  = 0;
    bool      _gifPlaying     = false;

    // ── Clock state ───────────────────────────────────────────────────────────
    bool _clockIs24h       = true;
    bool _clockShowSeconds = false;
    bool _clockShowDate    = false;

    // ── Internal helpers ──────────────────────────────────────────────────────
    void _blitRgb565(const uint8_t* buf, size_t len);
    void _drawClockFace();
    void _drawSmallDigits(int x, int y, const char* str,
                          uint16_t color565, uint8_t scale = 1);

    static void _taskEntry(void* param);
    void        _taskLoop();
};

// ── Singleton accessor ────────────────────────────────────────────────────────
DisplayManager& Display();