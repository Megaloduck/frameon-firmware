#include "display_manager.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ═══════════════════════════════════════════════════════════════════════════
//  display_manager.cpp
// ═══════════════════════════════════════════════════════════════════════════

// ── Singleton ─────────────────────────────────────────────────────────────────

static DisplayManager* _instance = nullptr;

DisplayManager& Display() {
    if (!_instance) _instance = new DisplayManager();
    return *_instance;
}

// ── Constructor ───────────────────────────────────────────────────────────────

DisplayManager::DisplayManager() {
    _mutex = xSemaphoreCreateMutex();

    // Allocate GIF frame pixel buffers in PSRAM
    for (int i = 0; i < GIF_MAX_FRAMES; i++) {
        if (psramFound()) {
            _gifFrames[i].pixels = (uint8_t*) ps_malloc(FRAME_BYTES);
        } else {
            _gifFrames[i].pixels = (uint8_t*) malloc(FRAME_BYTES);
        }
        _gifFrames[i].durationMs = 100;
    }
}

// ── begin() ───────────────────────────────────────────────────────────────────

bool DisplayManager::begin() {
    HUB75_I2S_CFG::i2s_pins pins = {
        HUB75_PIN_R1, HUB75_PIN_G1, HUB75_PIN_B1,
        HUB75_PIN_R2, HUB75_PIN_G2, HUB75_PIN_B2,
        HUB75_PIN_A,  HUB75_PIN_B,  HUB75_PIN_C,
        HUB75_PIN_D,  HUB75_PIN_E,
        HUB75_PIN_LAT, HUB75_PIN_OE, HUB75_PIN_CLK
    };

    HUB75_I2S_CFG cfg(
        MATRIX_COLS,   // width  (total incl. chain)
        MATRIX_ROWS,   // height
        CHAIN_LENGTH,  // chain length
        pins
    );

    cfg.double_buff = true;   // tear-free updates
    cfg.clkphase    = false;  // try true if you see ghost pixels

    _panel = new MatrixPanel_I2S_DMA(cfg);
    if (!_panel->begin()) {
        LOG("ERROR: MatrixPanel begin() failed");
        return false;
    }

    setBrightness(DEFAULT_BRIGHTNESS);
    _panel->clearScreen();

    LOG("Display initialised: %dx%d", MATRIX_COLS, MATRIX_ROWS);
    return true;
}

// ── setBrightness() ───────────────────────────────────────────────────────────

void DisplayManager::setBrightness(uint8_t value) {
    _brightness = value;
    // MatrixPanel uses 0–100; map from 0–255
    uint8_t mapped = (uint8_t)((value * 100UL) / 255UL);
    if (_panel) _panel->setBrightness8(mapped);
}

// ── showFrame() ───────────────────────────────────────────────────────────────

void DisplayManager::showFrame(const uint8_t* rgb565, size_t len) {
    if (!_panel) return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _blitRgb565(rgb565, len);
        _panel->flipDMABuffer();
        xSemaphoreGive(_mutex);
    }
}

// ── storeGifFrame() ───────────────────────────────────────────────────────────

bool DisplayManager::storeGifFrame(uint8_t index, const uint8_t* rgb565,
                                   size_t len, uint16_t durationMs) {
    if (index >= GIF_MAX_FRAMES) return false;
    if (!_gifFrames[index].pixels)  return false;

    size_t copy = (len < FRAME_BYTES) ? len : FRAME_BYTES;
    memcpy(_gifFrames[index].pixels, rgb565, copy);
    _gifFrames[index].durationMs = durationMs;
    return true;
}

void DisplayManager::setGifFrameCount(uint8_t count) {
    _gifFrameCount = (count > GIF_MAX_FRAMES) ? GIF_MAX_FRAMES : count;
}

// ── startGifPlayback() / stopGifPlayback() ────────────────────────────────────

void DisplayManager::startGifPlayback() {
    _gifPlaying    = true;
    _gifCurrentIdx = 0;
}

void DisplayManager::stopGifPlayback() {
    _gifPlaying = false;
}

// ── renderClock() ─────────────────────────────────────────────────────────────
// This is called by the clock task — it delegates to ClockManager which
// renders into a buffer, then we blit that buffer to the panel.

void DisplayManager::renderClock(bool is24h, bool showSeconds, bool showDate) {
    // Actual clock rendering lives in clock_manager.cpp.
    // This method exists so DisplayManager owns the mutex around the blit.
    (void)is24h; (void)showSeconds; (void)showDate;
    // ClockManager calls Display().showFrame() directly with its rendered buf.
}

// ── clear() ───────────────────────────────────────────────────────────────────

void DisplayManager::clear() {
    if (!_panel) return;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        _panel->clearScreen();
        _panel->flipDMABuffer();
        xSemaphoreGive(_mutex);
    }
    _gifPlaying = false;
}

// ── startTask() ───────────────────────────────────────────────────────────────

void DisplayManager::startTask() {
    xTaskCreatePinnedToCore(
        _taskEntry,
        "display_task",
        DISPLAY_TASK_STACK,
        this,
        TASK_PRIO_DISPLAY,
        nullptr,
        1   // pin to core 1 (core 0 runs WiFi/BLE)
    );
}

void DisplayManager::_taskEntry(void* param) {
    static_cast<DisplayManager*>(param)->_taskLoop();
}

// ── _taskLoop() ───────────────────────────────────────────────────────────────
// Handles GIF playback frame timing.

void DisplayManager::_taskLoop() {
    TickType_t lastWake = xTaskGetTickCount();

    for (;;) {
        if (_gifPlaying && _gifFrameCount > 0) {
            const GifFrame& f = _gifFrames[_gifCurrentIdx];

            // Blit frame to panel
            if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                _blitRgb565(f.pixels, FRAME_BYTES);
                _panel->flipDMABuffer();
                xSemaphoreGive(_mutex);
            }

            uint16_t delay = (f.durationMs > GIF_MIN_FRAME_MS)
                             ? f.durationMs
                             : GIF_MIN_FRAME_MS;

            _gifCurrentIdx = (_gifCurrentIdx + 1) % _gifFrameCount;
            vTaskDelay(pdMS_TO_TICKS(delay));
        } else {
            // Nothing to do — yield for 50 ms
            vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(50));
        }
    }
}

// ── _blitRgb565() ─────────────────────────────────────────────────────────────
// Convert big-endian RGB565 flat buffer → panel pixels.
// The panel's drawPixelRGB565() expects a native-endian uint16_t; the wire
// format from the Flutter app is big-endian, so we swap bytes here.

void DisplayManager::_blitRgb565(const uint8_t* buf, size_t len) {
    if (!_panel || !buf) return;

    size_t pixels = len / 2;
    if (pixels > (size_t)(MATRIX_COLS * MATRIX_ROWS)) {
        pixels = MATRIX_COLS * MATRIX_ROWS;
    }

    for (size_t i = 0; i < pixels; i++) {
        // Reconstruct native-endian u16 from big-endian bytes
        uint16_t color = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
        int col = i % MATRIX_COLS;
        int row = i / MATRIX_COLS;
        _panel->drawPixelRGB565(col, row, color);
    }
}