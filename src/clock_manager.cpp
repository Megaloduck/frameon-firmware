#include "clock_manager.h"
#include "display_manager.h"
#include <sys/time.h>
#include <time.h>

// ═══════════════════════════════════════════════════════════════════════════
//  clock_manager.cpp
//
//  Clock face renders directly into a FRAME_BYTES RGB565 buffer which is
//  then handed to Display().showFrame().  No external RTC chip needed —
//  the ESP32 keeps time via gettimeofday() after sync from the phone.
// ═══════════════════════════════════════════════════════════════════════════

// ── Singleton ─────────────────────────────────────────────────────────────────

static ClockManager* _clkInstance = nullptr;

ClockManager& Clock() {
    if (!_clkInstance) _clkInstance = new ClockManager();
    return *_clkInstance;
}

// ── 3×5 pixel font ───────────────────────────────────────────────────────────
// Characters: 0-9, colon (:), slash (/), space, A, M, P
// Each entry is 5 bytes: one per row, MSB = leftmost pixel (3 bits wide).

const uint8_t ClockManager::_font3x5[][5] = {
    //  row0  row1  row2  row3  row4
    { 0b111, 0b101, 0b101, 0b101, 0b111 }, // 0
    { 0b010, 0b110, 0b010, 0b010, 0b111 }, // 1
    { 0b111, 0b001, 0b111, 0b100, 0b111 }, // 2
    { 0b111, 0b001, 0b111, 0b001, 0b111 }, // 3
    { 0b101, 0b101, 0b111, 0b001, 0b001 }, // 4
    { 0b111, 0b100, 0b111, 0b001, 0b111 }, // 5
    { 0b111, 0b100, 0b111, 0b101, 0b111 }, // 6
    { 0b111, 0b001, 0b001, 0b001, 0b001 }, // 7
    { 0b111, 0b101, 0b111, 0b101, 0b111 }, // 8
    { 0b111, 0b101, 0b111, 0b001, 0b111 }, // 9
    { 0b000, 0b010, 0b000, 0b010, 0b000 }, // : (index 10)
    { 0b001, 0b001, 0b010, 0b100, 0b100 }, // / (index 11)
    { 0b000, 0b000, 0b000, 0b000, 0b000 }, // space (index 12)
    { 0b010, 0b101, 0b111, 0b101, 0b101 }, // A (index 13)
    { 0b101, 0b111, 0b101, 0b101, 0b101 }, // M (index 14)
    { 0b111, 0b101, 0b111, 0b100, 0b100 }, // P (index 15)
};

int ClockManager::_charIndex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c == ':') return 10;
    if (c == '/') return 11;
    if (c == ' ') return 12;
    if (c == 'A') return 13;
    if (c == 'M') return 14;
    if (c == 'P') return 15;
    return 12; // space fallback
}

// ── Constructor ───────────────────────────────────────────────────────────────

ClockManager::ClockManager() {}

// ── syncTime() ───────────────────────────────────────────────────────────────

void ClockManager::syncTime(const Config& cfg) {
    _cfg = cfg;

    // Set the ESP32 system time
    struct timeval tv;
    tv.tv_sec  = cfg.epochUtc;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);

    LOG("System time set to epoch %lu", (unsigned long)cfg.epochUtc);
}

// ── startTask() / stopTask() ──────────────────────────────────────────────────

void ClockManager::startTask() {
    if (_taskHandle) return; // already running
    _stop = false;
    xTaskCreatePinnedToCore(
        _taskEntry,
        "clock_task",
        4096,
        this,
        TASK_PRIO_CLOCK,
        &_taskHandle,
        1   // core 1
    );
    LOG("Clock task started");
}

void ClockManager::stopTask() {
    if (!_taskHandle) return;
    _stop = true;
    // Give the task time to notice and self-terminate
    vTaskDelay(pdMS_TO_TICKS(CLOCK_REFRESH_MS + 50));
    _taskHandle = nullptr;
    LOG("Clock task stopped");
}

void ClockManager::_taskEntry(void* param) {
    static_cast<ClockManager*>(param)->_taskLoop();
}

// ── _taskLoop() ───────────────────────────────────────────────────────────────

void ClockManager::_taskLoop() {
    // Allocate render buffer (PSRAM preferred)
    uint8_t* buf = psramFound()
        ? (uint8_t*) ps_malloc(FRAME_BYTES)
        : (uint8_t*) malloc(FRAME_BYTES);

    if (!buf) {
        LOG("Clock task: failed to allocate render buffer");
        _taskHandle = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    while (!_stop) {
        renderToBuffer(buf);
        Display().showFrame(buf, FRAME_BYTES);
        vTaskDelay(pdMS_TO_TICKS(CLOCK_REFRESH_MS));
    }

    free(buf);
    _taskHandle = nullptr;
    vTaskDelete(nullptr);
}

// ── renderToBuffer() ─────────────────────────────────────────────────────────

void ClockManager::renderToBuffer(uint8_t* out) {
    // Black background
    memset(out, 0, FRAME_BYTES);

    // Get current time
    time_t    now = time(nullptr);
    struct tm tm  = {};
    localtime_r(&now, &tm);

    // ── Time string ───────────────────────────────────────────────────────────
    // We build the string ourselves to avoid printf overhead in a tight loop.
    char timeBuf[12] = {};
    int  h = tm.tm_hour;
    bool pm = false;

    if (!_cfg.is24h) {
        pm = (h >= 12);
        h  = h % 12;
        if (h == 0) h = 12;
    }

    // HH:MM or HH:MM:SS
    int pos = 0;
    timeBuf[pos++] = '0' + h / 10;
    timeBuf[pos++] = '0' + h % 10;
    timeBuf[pos++] = ':';
    timeBuf[pos++] = '0' + tm.tm_min / 10;
    timeBuf[pos++] = '0' + tm.tm_min % 10;

    if (_cfg.showSeconds) {
        timeBuf[pos++] = ':';
        timeBuf[pos++] = '0' + tm.tm_sec / 10;
        timeBuf[pos++] = '0' + tm.tm_sec % 10;
    }
    timeBuf[pos] = '\0';

    // ── Colours (RGB565) ──────────────────────────────────────────────────────
    // Green  0x07E0   Cyan  0x07FF   White 0xFFFF   Amber 0xFD20
    uint16_t timeColor = 0x07E0;  // neon green — matches Flutter default
    uint16_t dimColor  = 0x0320;  // dim green for date / AM-PM

    // ── Layout ────────────────────────────────────────────────────────────────
    // Each digit glyph is 3px wide, gap 1px → 4px per char.
    // Colon is also 3px wide. Scale 2 doubles everything.
    int scale = 2; // glyph pixels → 2×2 screen pixels each

    // Effective character width and height at this scale
    int charW = 3 * scale;
    int charH = 5 * scale;
    int gap   = 1 * scale;

    // Calculate string pixel width for centering
    int strLen   = strlen(timeBuf);
    int totalW   = strLen * (charW + gap) - gap;
    int startX   = (MATRIX_COLS - totalW) / 2;
    int startY   = _cfg.showDate
                   ? (MATRIX_ROWS / 2 - charH - scale)    // move up if date shown
                   : (MATRIX_ROWS - charH) / 2;           // vertically centered

    // Draw time string
    int cx = startX;
    for (int i = 0; i < strLen; i++) {
        _drawChar(out, cx, startY, timeBuf[i], timeColor, scale);
        cx += charW + gap;
    }

    // ── AM/PM indicator (12-h mode) ───────────────────────────────────────────
    if (!_cfg.is24h) {
        char ampm[3] = { (char)(pm ? 'P' : 'A'), 'M', '\0' };
        int ax = cx + scale * 2;                    // right of time
        int ay = startY + charH - 5 * (scale / 2); // bottom-aligned
        _drawChar(out, ax,     ay, ampm[0], dimColor, scale / 2 + 1);
        _drawChar(out, ax + 4, ay, ampm[1], dimColor, scale / 2 + 1);
    }

    // ── Date line ─────────────────────────────────────────────────────────────
    if (_cfg.showDate) {
        // Format: DD/MM/YYYY  — compact for the 64px width
        char dateBuf[12] = {};
        snprintf(dateBuf, sizeof(dateBuf), "%02d/%02d/%04d",
                 tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900);

        // Date at scale 1 (small)
        int dScale = 1;
        int dCharW = 3 * dScale;
        int dGap   = 1 * dScale;
        int dLen   = strlen(dateBuf);
        int dTotalW = dLen * (dCharW + dGap) - dGap;
        int dx = (MATRIX_COLS - dTotalW) / 2;
        int dy = startY + charH + scale * 2;  // below clock

        for (int i = 0; i < dLen; i++) {
            _drawChar(out, dx, dy, dateBuf[i], dimColor, dScale);
            dx += dCharW + dGap;
        }

        // Horizontal separator line between time and date
        int lineY  = startY + charH + scale;
        int lineX0 = MATRIX_COLS / 4;
        int lineX1 = MATRIX_COLS * 3 / 4;
        for (int x = lineX0; x <= lineX1; x++) {
            _setPixel(out, x, lineY, (uint16_t)(dimColor & 0x0F00));
        }
    }
}

// ── _drawChar() ──────────────────────────────────────────────────────────────

void ClockManager::_drawChar(uint8_t* buf, int x, int y,
                              char c, uint16_t color565, int scale) {
    int idx = _charIndex(c);

    for (int row = 0; row < 5; row++) {
        uint8_t mask = _font3x5[idx][row];
        for (int col = 0; col < 3; col++) {
            bool lit = (mask >> (2 - col)) & 1;
            if (!lit) continue;

            // Scale: draw scale×scale block per font pixel
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    int py = y + row * scale + sy;
                    if (px >= 0 && px < MATRIX_COLS &&
                        py >= 0 && py < MATRIX_ROWS) {
                        _setPixel(buf, px, py, color565);
                    }
                }
            }
        }
    }
}

// ── _setPixel() ───────────────────────────────────────────────────────────────

void ClockManager::_setPixel(uint8_t* buf, int col, int row, uint16_t color565) {
    size_t idx = ((size_t)row * MATRIX_COLS + col) * 2;
    if (idx + 1 >= FRAME_BYTES) return;
    buf[idx]     = (color565 >> 8) & 0xFF;   // big-endian, matches wire format
    buf[idx + 1] =  color565       & 0xFF;
}