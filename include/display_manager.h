#pragma once

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <NimBLEDevice.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════
//  display_manager.h
//
//  Single owner of the HUB75 DMA panel.  All other modules call into this
//  instead of touching the panel directly.
//
//  Thread-safety: every public method that touches the panel acquires
//  _mutex, so it is safe to call from both the BLE task and the clock task.
//
//  FIX #3: GIF frame pixel buffers are now allocated lazily in
//  storeGifFrame() rather than all upfront in the constructor.
//  This avoids consuming 256 KB of PSRAM/SRAM on devices that never
//  receive a GIF.
// ═══════════════════════════════════════════════════════════════════════════

// ── GIF frame descriptor ─────────────────────────────────────────────────────

struct GifFrame {
    uint8_t* pixels;    // FRAME_BYTES of RGB565 (big-endian); nullptr until first write
    uint16_t durationMs;
};

// ── Display manager ───────────────────────────────────────────────────────────

class DisplayManager {
public:
    DisplayManager();

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    bool begin();
    void setBrightness(uint8_t value);  // 0–255 → mapped to panel 0–100

    // ── Still frame ───────────────────────────────────────────────────────────
    // Push one full FRAME_BYTES buffer of big-endian RGB565 to the panel.
    void showFrame(const uint8_t* rgb565, size_t len);

    // ── GIF playback ─────────────────────────────────────────────────────────
    // Store a decoded GIF frame.  Allocates the pixel buffer on first write
    // for each slot (FIX #3).  Returns false if index is out of range or
    // allocation fails.
    bool storeGifFrame(uint8_t index, const uint8_t* rgb565,
                       size_t len, uint16_t durationMs);
    void setGifFrameCount(uint8_t count);
    void startGifPlayback();
    void stopGifPlayback();

    // ── Clock mode ────────────────────────────────────────────────────────────
    void renderClock(bool is24h, bool showSeconds, bool showDate);

    // ── Utilities ─────────────────────────────────────────────────────────────
    void clear();
    MatrixPanel_I2S_DMA* panel() { return _panel; }

    void startTask();

private:
    MatrixPanel_I2S_DMA* _panel = nullptr;
    SemaphoreHandle_t     _mutex;
    uint8_t               _brightness = DEFAULT_BRIGHTNESS;

    // ── GIF state ─────────────────────────────────────────────────────────────
    // FIX #3: pixels pointers start nullptr; allocated lazily in storeGifFrame.
    GifFrame  _gifFrames[GIF_MAX_FRAMES];
    uint8_t   _gifFrameCount  = 0;
    uint8_t   _gifCurrentIdx  = 0;
    bool      _gifPlaying     = false;

    // ── Clock state ───────────────────────────────────────────────────────────
    bool _clockIs24h       = true;
    bool _clockShowSeconds = false;
    bool _clockShowDate    = false;

    // ── Internal helpers ──────────────────────────────────────────────────────
    // FIX #7: _blitRgb565 now uses drawRGBBitmap for a single DMA-friendly
    // call instead of 2048 individual drawPixelRGB565 calls.
    void _blitRgb565(const uint8_t* buf, size_t len);
    void _drawClockFace();
    void _drawSmallDigits(int x, int y, const char* str,
                          uint16_t color565, uint8_t scale = 1);

    static void _taskEntry(void* param);
    void        _taskLoop();
};

DisplayManager& Display();