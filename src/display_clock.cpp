#include "display_clock.h"
#include "matrix.h"
#include "api_server.h"   // g_clockCfg
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

static WiFiUDP   _udp;
static NTPClient *_ntp = nullptr;
static uint32_t  _lastDraw  = 0;
static uint32_t  _lastSync  = 0;
static int       _prevSecond = -1;

// Tiny 5×3 bitmap font digits (0–9) and colon
// Each digit: 5 rows × 3 bits (MSB = left pixel)
static const uint8_t FONT5x3[11][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b001, 0b001, 0b001}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}, // 9
    {0b000, 0b010, 0b000, 0b010, 0b000}, // : (index 10)
};

static void _drawChar(int x, int y, int idx, uint16_t color) {
    if (idx < 0 || idx > 10) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = FONT5x3[idx][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (0b100 >> col)) {
                matrix->drawPixel(x + col, y + row, color);
            }
        }
    }
}

static void _drawDigit(int x, int y, int digit, uint16_t color) {
    _drawChar(x, y, digit, color);
}

static void _drawColon(int x, int y, uint16_t color) {
    _drawChar(x, y, 10, color);
}

// Draw a 2-digit number at (x,y)
static void _draw2Digits(int x, int y, int val, uint16_t color) {
    _drawDigit(x,     y, (val / 10) % 10, color);
    _drawDigit(x + 4, y, val % 10,        color);
}

// Tiny 4×3 characters for date text (A-Z subset)
// Using matrix's built-in font for date line (1px height = 8px)
static void _drawDateLine(const struct tm &t) {
    static const char *days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                   "JUL","AUG","SEP","OCT","NOV","DEC"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %02d %s",
             days[t.tm_wday], t.tm_mday, months[t.tm_mon]);

    // Draw using the matrix built-in tiny font at the bottom row
    matrix->setTextSize(1);
    matrix->setTextColor(COL_BLUE);
    // Centre the string: each char = 6px wide with default font
    int len   = strlen(buf);
    int textW = len * 4; // approx width with small font
    int startX = (PANEL_WIDTH - textW) / 2;
    matrix->setCursor(startX, 24);
    matrix->print(buf);
}

void clock_init(const char *ntpServer) {
    if (_ntp) { delete _ntp; _ntp = nullptr; }
    _ntp = new NTPClient(_udp, ntpServer, 0, NTP_UPDATE_INTERVAL);
    _ntp->begin();
    _ntp->update();
    Serial.printf("[Clock] NTP init: %s\n", ntpServer);
}

void clock_draw() {
    uint32_t now = millis();

    // NTP sync every NTP_UPDATE_INTERVAL ms
    if (_ntp && now - _lastSync > NTP_UPDATE_INTERVAL) {
        _ntp->update();
        _lastSync = now;
    }

    // Redraw only when seconds change (saves CPU + reduces flicker)
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;

    if (timeinfo.tm_sec == _prevSecond) return;
    _prevSecond = timeinfo.tm_sec;

    matrix->clearScreen();

    int hour = g_clockCfg.is24h
        ? timeinfo.tm_hour
        : (timeinfo.tm_hour % 12 == 0 ? 12 : timeinfo.tm_hour % 12);

    uint16_t timeColor = COL_GREEN;
    uint16_t colonColor = (timeinfo.tm_sec % 2 == 0) ? COL_GREEN : COL_DIM;

    // Layout (32px tall, 64px wide):
    // Time row centred vertically at y=8 (5px tall digits + spacing)
    // HH:MM  — x positions calculated for centre alignment
    //   HH = 2 digits × 4px + gap = ~11px
    //   :  = 3px + 1px gap
    //   MM = 11px
    //   total ≈ 27px  → startX = (64-27)/2 = 18

    if (g_clockCfg.showSeconds) {
        // HH:MM:SS — smaller layout
        int y = 6;
        _draw2Digits(2,  y, hour,             timeColor);
        _drawColon  (13, y,                   colonColor);
        _draw2Digits(16, y, timeinfo.tm_min,  timeColor);
        _drawColon  (27, y,                   colonColor);
        _draw2Digits(30, y, timeinfo.tm_sec,  COL_DIM);
    } else {
        // HH:MM — larger, centred
        int y = 9;
        // Use bigger rendering: double-size digits via two-pass
        // We'll use the 5x3 font at 2× scale manually
        auto draw2x = [&](int x, int yy, int idx, uint16_t col) {
            if (idx < 0 || idx > 10) return;
            for (int row = 0; row < 5; row++) {
                uint8_t bits = FONT5x3[idx][row];
                for (int c = 0; c < 3; c++) {
                    if (bits & (0b100 >> c)) {
                        matrix->drawPixel(x + c*2,   yy + row*2,   col);
                        matrix->drawPixel(x + c*2+1, yy + row*2,   col);
                        matrix->drawPixel(x + c*2,   yy + row*2+1, col);
                        matrix->drawPixel(x + c*2+1, yy + row*2+1, col);
                    }
                }
            }
        };

        // HH: each digit = 6px wide (3px × 2), gap 1px
        // Total: 6+1+6 + colon(2+1) + 6+1+6 = 29px → start at (64-29)/2 = 17
        int x = 17;
        draw2x(x,    y, (hour / 10) % 10, timeColor);  x += 7;
        draw2x(x,    y, hour % 10,        timeColor);  x += 7;
        // colon (2×2 dots)
        if (timeinfo.tm_sec % 2 == 0) {
            matrix->drawPixel(x, y+2, colonColor);
            matrix->drawPixel(x, y+3, colonColor);
            matrix->drawPixel(x, y+6, colonColor);
            matrix->drawPixel(x, y+7, colonColor);
        }
        x += 3;
        draw2x(x, y, (timeinfo.tm_min / 10) % 10, timeColor); x += 7;
        draw2x(x, y, timeinfo.tm_min % 10,        timeColor);

        // AM/PM indicator for 12h mode
        if (!g_clockCfg.is24h) {
            matrix->setTextSize(1);
            matrix->setTextColor(COL_DIM);
            matrix->setCursor(55, 9);
            matrix->print(timeinfo.tm_hour < 12 ? "A" : "P");
            matrix->setCursor(55, 15);
            matrix->print("M");
        }
    }

    // Date line
    if (g_clockCfg.showDate) {
        _drawDateLine(timeinfo);
    }

    matrix->flushDMABuffer();
}
