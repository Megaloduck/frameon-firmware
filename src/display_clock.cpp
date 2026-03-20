#include "display_clock.h"
#include "matrix.h"
#include "api_server.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>

static WiFiUDP    _udp;
static NTPClient *_ntp        = nullptr;
static uint32_t   _lastSync   = 0;
static int        _prevSecond = -1;

// 5-row × 3-col bitmap font (digits 0–9, colon at index 10)
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
    {0b000, 0b010, 0b000, 0b010, 0b000}, // :
};

static void _drawChar(int x, int y, int idx, uint16_t color) {
    if (idx < 0 || idx > 10) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = FONT5x3[idx][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (0b100 >> col)) matrix->drawPixel(x + col, y + row, color);
        }
    }
}

static void _draw2Digits(int x, int y, int val, uint16_t color) {
    _drawChar(x,     y, (val / 10) % 10, color);
    _drawChar(x + 4, y, val % 10,        color);
}

static void _drawDateLine(const struct tm &t) {
    static const char *days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
    static const char *months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                   "JUL","AUG","SEP","OCT","NOV","DEC"};
    char buf[16];
    snprintf(buf, sizeof(buf), "%s %02d %s",
             days[t.tm_wday], t.tm_mday, months[t.tm_mon]);

    int textW  = strlen(buf) * 4;
    int startX = (PANEL_WIDTH - textW) / 2;
    int dateY  = REAL_HEIGHT - 8; // 24

    matrix->setTextSize(1);
    matrix->setTextColor(COL_BLUE());
    matrix->setCursor(startX, dateY);
    matrix->print(buf);
}

void clock_init(const char *ntpServer) {
    if (_ntp) { delete _ntp; _ntp = nullptr; }
    _ntp = new NTPClient(_udp, ntpServer, 0, NTP_UPDATE_INTERVAL);
    _ntp->begin();
    // FIX: do NOT call _ntp->update() here.
    // _ntp->update() is a blocking UDP call that can take 1-2 seconds,
    // which prevents usb_api_loop() from running and causes 8s timeouts
    // on any USB command sent immediately after WiFi connects.
    // The first clock_draw() call will trigger forceUpdate() via the
    // _lastSync == 0 check, which is the correct non-blocking init path.
    _lastSync   = 0;    // force update on first clock_draw() call
    _prevSecond = -1;   // force redraw on first clock_draw() call
    Serial.printf("[Clock] NTP init (deferred sync): %s\n", ntpServer);
}

void clock_draw() {
    // Sync NTP if enough time has passed (or on first call when _lastSync==0)
    if (_ntp && (millis() - _lastSync > NTP_UPDATE_INTERVAL || _lastSync == 0)) {
        _ntp->update(); // non-blocking on subsequent calls; may block once at start
        _lastSync = millis();
    }

    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return;
    if (timeinfo.tm_sec == _prevSecond) return;
    _prevSecond = timeinfo.tm_sec;

    matrix->clearScreen();

    int hour = g_clockCfg.is24h
        ? timeinfo.tm_hour
        : (timeinfo.tm_hour % 12 == 0 ? 12 : timeinfo.tm_hour % 12);

    uint16_t timeColor  = COL_GREEN();
    uint16_t colonColor = (timeinfo.tm_sec % 2 == 0) ? COL_GREEN() : COL_DIM();

    if (g_clockCfg.showSeconds) {
        int y = 6;
        _draw2Digits(2,  y, hour,            timeColor);
        _drawChar   (13, y, 10,              colonColor);
        _draw2Digits(16, y, timeinfo.tm_min, timeColor);
        _drawChar   (27, y, 10,              colonColor);
        _draw2Digits(30, y, timeinfo.tm_sec, COL_DIM());
    } else {
        int y = 9;
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

        int x = 13;
        draw2x(x, y, (hour / 10) % 10,            timeColor); x += 7;
        draw2x(x, y, hour % 10,                   timeColor); x += 7;

        if (timeinfo.tm_sec % 2 == 0) {
            matrix->drawPixel(x, y+2, colonColor);
            matrix->drawPixel(x, y+3, colonColor);
            matrix->drawPixel(x, y+6, colonColor);
            matrix->drawPixel(x, y+7, colonColor);
        }
        x += 3;

        draw2x(x, y, (timeinfo.tm_min / 10) % 10, timeColor); x += 7;
        draw2x(x, y, timeinfo.tm_min % 10,        timeColor);

        if (!g_clockCfg.is24h) {
            matrix->setTextSize(1);
            matrix->setTextColor(COL_DIM());
            matrix->setCursor(57, 9);
            matrix->print(timeinfo.tm_hour < 12 ? "A" : "P");
            matrix->setCursor(57, 16);
            matrix->print("M");
        }
    }

    if (g_clockCfg.showDate) _drawDateLine(timeinfo);

    matrix->flipDMABuffer();
}
