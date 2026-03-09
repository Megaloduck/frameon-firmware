#include "display_manager.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ═══════════════════════════════════════════════════════════════════════════
//  display_manager.cpp
//
//  FIX #2: _taskLoop() stale lastWake bug.
//    Previously lastWake was captured once before the loop.  When
//    _gifPlaying was true the task used vTaskDelay (which does NOT update
//    lastWake), so switching back to the idle branch called
//    vTaskDelayUntil with a lastWake that was potentially seconds old,
//    causing a burst of zero-delay ticks.
//    Fix: refresh lastWake with xTaskGetTickCount() at the top of every
//    iteration so both branches always have a current baseline.
//
//  FIX #3: Lazy GIF frame buffer allocation.
//    Constructor no longer allocates 64×4096 = 256 KB upfront.  Each slot's
//    pixel buffer is allocated the first time storeGifFrame() is called for
//    that index.  Devices that never receive a GIF pay nothing.
//
//  FIX #7: Fast blit via swap buffer + drawRGBBitmap.
//    _blitRgb565 previously called drawPixelRGB565() 2048 times per frame.
//    We now byte-swap the entire buffer into a temporary native-endian
//    uint16_t[] and hand it to drawRGBBitmap() in a single call, which the
//    DMA driver can pipeline much more efficiently.
// ═══════════════════════════════════════════════════════════════════════════

// ── Singleton ─────────────────────────────────────────────────────────────────

static DisplayManager* _instance = nullptr;

DisplayManager& Display() {
    if (!_instance) _instance = new DisplayManager();
    return *_instance;
}

// ── Constructor ───────────────────────────────────────────────────────────────
// FIX #3: Do NOT allocate GIF pixel buffers here.  Zero-initialise the array
// so every pixels pointer starts as nullptr.

DisplayManager::DisplayManager() {
    _mutex = xSemaphoreCreateMutex();

    for (int i = 0; i < GIF_MAX_FRAMES; i++) {
        _gifFrames[i].pixels     = nullptr;  // allocated lazily
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
        MATRIX_COLS,
        MATRIX_ROWS,
        CHAIN_LENGTH,
        pins
    );

    cfg.double_buff = true;
    cfg.clkphase    = false;

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
// FIX #3: Allocate the pixel buffer on first use for each slot.

bool DisplayManager::storeGifFrame(uint8_t index, const uint8_t* rgb565,
                                   size_t len, uint16_t durationMs) {
    if (index >= GIF_MAX_FRAMES) return false;

    // Lazy allocation — only pay for what is actually used.
    if (_gifFrames[index].pixels == nullptr) {
        _gifFrames[index].pixels = psramFound()
            ? (uint8_t*) ps_malloc(FRAME_BYTES)
            : (uint8_t*) malloc(FRAME_BYTES);

        if (!_gifFrames[index].pixels) {
            LOG("ERROR: failed to allocate GIF frame %d buffer", index);
            return false;
        }
        LOG("GIF frame %d buffer allocated (%u bytes)", index, FRAME_BYTES);
    }

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

void DisplayManager::renderClock(bool is24h, bool showSeconds, bool showDate) {
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
        1
    );
}

void DisplayManager::_taskEntry(void* param) {
    static_cast<DisplayManager*>(param)->_taskLoop();
}

// ── _taskLoop() ───────────────────────────────────────────────────────────────
// FIX #2: Refresh lastWake at the top of every iteration.
//
// Old code set lastWake once before the loop.  The GIF branch used
// vTaskDelay which does NOT advance lastWake, so when the idle branch
// resumed it called vTaskDelayUntil with a stale baseline and fired
// immediately (or many times in a burst) until it caught up.
//
// New code: capture xTaskGetTickCount() at the start of every loop body.
// The idle branch uses vTaskDelayUntil from that fresh baseline, giving a
// consistent 50 ms yield regardless of whether GIF playback was active.

void DisplayManager::_taskLoop() {
    for (;;) {
        // FIX #2: Always get a fresh tick baseline at the top of the loop.
        TickType_t loopStart = xTaskGetTickCount();

        if (_gifPlaying && _gifFrameCount > 0) {
            const GifFrame& f = _gifFrames[_gifCurrentIdx];

            // Guard: skip if this slot was never allocated (shouldn't happen
            // in normal use, but protects against partial GIF transfers).
            if (f.pixels != nullptr) {
                if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                    _blitRgb565(f.pixels, FRAME_BYTES);
                    _panel->flipDMABuffer();
                    xSemaphoreGive(_mutex);
                }
            }

            uint16_t delay = (f.durationMs > GIF_MIN_FRAME_MS)
                             ? f.durationMs
                             : GIF_MIN_FRAME_MS;

            _gifCurrentIdx = (_gifCurrentIdx + 1) % _gifFrameCount;

            // Use vTaskDelayUntil from loopStart so frame timing accounts
            // for the time spent blitting.
            vTaskDelayUntil(&loopStart, pdMS_TO_TICKS(delay));
        } else {
            // FIX #2: loopStart is always fresh, so this 50 ms yield is exact.
            vTaskDelayUntil(&loopStart, pdMS_TO_TICKS(50));
        }
    }
}

// ── _blitRgb565() ─────────────────────────────────────────────────────────────
// FIX #7: Batch blit using a swap buffer + drawRGBBitmap.
//
// Old code called drawPixelRGB565() in a tight loop (2048 calls for 64×32).
// Each call goes through the panel driver's coordinate mapping and write
// path individually, which is expensive.
//
// New code:
//   1. Byte-swap the entire big-endian wire buffer into a stack-allocated
//      native-endian uint16_t array.
//   2. Call drawRGBBitmap() once — the DMA driver can pipeline this as a
//      single contiguous write, dramatically reducing CPU overhead.
//
// Stack cost: MATRIX_COLS * MATRIX_ROWS * 2 = 4096 bytes.
// This is within the display task's 4096-byte stack only if no other large
// locals exist in this call chain.  If stack pressure is a concern, promote
// _swapBuf to a member (allocated in begin()) at the cost of 4 KB SRAM.

void DisplayManager::_blitRgb565(const uint8_t* buf, size_t len) {
    if (!_panel || !buf) return;

    const size_t totalPixels = MATRIX_COLS * MATRIX_ROWS;
    size_t       pixels      = len / 2;
    if (pixels > totalPixels) pixels = totalPixels;

    // Allocate swap buffer — prefer PSRAM to keep stack usage low.
    uint16_t* swapBuf = psramFound()
        ? (uint16_t*) ps_malloc(totalPixels * sizeof(uint16_t))
        : (uint16_t*) malloc(totalPixels * sizeof(uint16_t));

    if (!swapBuf) {
        // Fallback: pixel-by-pixel (original behaviour) if allocation fails.
        LOG("WARN: _blitRgb565 swap alloc failed — falling back to slow path");
        for (size_t i = 0; i < pixels; i++) {
            uint16_t color = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
            _panel->drawPixelRGB565(i % MATRIX_COLS, i / MATRIX_COLS, color);
        }
        return;
    }

    // Byte-swap big-endian wire format → native-endian uint16_t.
    for (size_t i = 0; i < pixels; i++) {
        swapBuf[i] = ((uint16_t)buf[i * 2] << 8) | buf[i * 2 + 1];
    }
    // Pad any trailing pixels (shouldn't happen with a complete frame).
    for (size_t i = pixels; i < totalPixels; i++) {
        swapBuf[i] = 0;
    }

    // Single call — much faster than 2048 individual writes.
    _panel->drawRGBBitmap(0, 0, swapBuf, MATRIX_COLS, MATRIX_ROWS);

    free(swapBuf);
}