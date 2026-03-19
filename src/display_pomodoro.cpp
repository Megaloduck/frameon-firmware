#include "display_pomodoro.h"
#include "matrix.h"
#include "api_server.h"
#include <math.h>

static int _prevSecond = -1;

// Draw a circular progress arc on the 32×32 left half of the panel
// (right half shows the time digits)
static void _drawArc(float progress, uint16_t color) {
    // Centre of the left 32×32 area
    const float cx = 15.5f, cy = 15.5f, r = 13.0f;
    const int   steps = 360;

    for (int i = 0; i < steps; i++) {
        float angle = (i / (float)steps) * 2.0f * M_PI - M_PI / 2.0f;
        int   x     = (int)(cx + r * cosf(angle));
        int   y     = (int)(cy + r * sinf(angle));

        if (x >= 0 && x < 32 && y >= 0 && y < 32) {
            uint16_t c = (i / (float)steps <= progress) ? color : COL_DIM;
            matrix->drawPixel(x, y, c);
        }
    }
}

void pomodoro_draw() {
    if (g_pomoTimer.secondsRemaining == _prevSecond) return;
    _prevSecond = g_pomoTimer.secondsRemaining;

    matrix->clearScreen();

    // Phase colour
    uint16_t phaseColor;
    const char *phaseLabel;
    int totalSecs;

    switch (g_pomoTimer.phase) {
        case POMO_SHORT_BREAK:
            phaseColor = COL_GREEN;
            phaseLabel = "BRK";
            totalSecs  = g_pomoCfg.shortBreakMinutes * 60;
            break;
        case POMO_LONG_BREAK:
            phaseColor = COL_BLUE;
            phaseLabel = "LNG";
            totalSecs  = g_pomoCfg.longBreakMinutes * 60;
            break;
        default: // POMO_WORK
            phaseColor = COL_ORANGE;
            phaseLabel = "FOC";
            totalSecs  = g_pomoCfg.workMinutes * 60;
            break;
    }

    float progress = totalSecs > 0
        ? (float)g_pomoTimer.secondsRemaining / totalSecs
        : 0.0f;

    // Arc on left 32×32
    _drawArc(progress, phaseColor);

    // Countdown digits on right side (x=34..63)
    int mins = g_pomoTimer.secondsRemaining / 60;
    int secs = g_pomoTimer.secondsRemaining % 60;

    matrix->setTextSize(1);
    matrix->setTextColor(phaseColor);

    // MM:SS
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", mins, secs);
    matrix->setCursor(34, 11);
    matrix->print(buf);

    // Phase label
    matrix->setTextColor(COL_DIM);
    matrix->setCursor(37, 21);
    matrix->print(phaseLabel);

    // Running indicator dot
    if (g_pomoTimer.running) {
        matrix->drawPixel(60, 2, phaseColor);
    }

    // Session dots across top of arc area
    for (int i = 0; i < g_pomoCfg.sessionsBeforeLong; i++) {
        uint16_t dotColor = (i < g_pomoTimer.sessionsCompleted % g_pomoCfg.sessionsBeforeLong)
            ? phaseColor : COL_DIM;
        matrix->drawPixel(6 + i * 5, 1, dotColor);
    }

    matrix->flushDMABuffer();
}
