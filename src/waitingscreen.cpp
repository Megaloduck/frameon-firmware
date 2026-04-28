// ─────────────────────────────────────────────────────────────────────────────
// waitingscreen.cpp — Idle splash shown when no animation is loaded
//
// Displayed by displayTask (Core 0) whenever activeFrameCount == 0.
//
// Phase 1 (0–3 s): "FRAMEON" text + boot progress bar filling left-to-right.
// Phase 2 (3 s+):  "READY" text + bouncing dot that changes color on wall hit.
// ─────────────────────────────────────────────────────────────────────────────

#include "waitingscreen.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Forward-declared in main.cpp — shared singleton matrix handle.
extern MatrixPanel_I2S_DMA* matrix;

void showWaitingScreen(uint32_t elapsedMs) {
    matrix->fillScreen(0);  // clears back buffer only (not both like clearScreen)

    const uint32_t bootDuration = 3000; // ms for Phase 1

    const uint16_t green = matrix->color565(33, 195, 44);
    const uint16_t white = matrix->color565(255, 255, 255);
    const uint16_t dim   = matrix->color565(15, 60, 18);

    matrix->setTextSize(1);

    // ─────────────────────────────────────────────────────────────────────────
    // Phase 1: boot progress bar
    // ─────────────────────────────────────────────────────────────────────────
    if (elapsedMs < bootDuration) {
        // Centered "FRAMEON" — 7 chars × ~6 px = 42 px wide
        matrix->setTextColor(green);
        int textX = (64 - 7 * 6) / 2;
        matrix->setCursor(textX, 8);
        matrix->print("FRAMEON");

        float progress = (float)elapsedMs / (float)bootDuration;

        const int barWidth  = 50;
        const int barHeight = 4;
        const int barX      = (64 - barWidth) / 2;
        const int barY      = 22;

        // Border
        matrix->drawRect(barX, barY, barWidth, barHeight, dim);

        // Fill
        int fillWidth = (int)(progress * (barWidth - 2));
        if (fillWidth > 0) {
            matrix->fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, white);
        }
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Phase 2: bouncing dot + "READY"
    // ─────────────────────────────────────────────────────────────────────────
    else {
        static float    x        = 32.0f;
        static float    y        = 16.0f;
        static float    vx       = 1.0f;
        static float    vy       = 1.0f;
        static uint16_t dotColor = 0xFFFF; // white initially

        const int radius = 2;
        const int minX   = radius;
        const int maxX   = 64 - radius;
        const int minY   = radius;
        const int maxY   = 32 - radius;

        x += vx;
        y += vy;

        bool hitWall = false;

        if (x <= minX) { x = minX; vx = -vx; hitWall = true; }
        else if (x >= maxX) { x = maxX; vx = -vx; hitWall = true; }

        if (y <= minY) { y = minY; vy = -vy; hitWall = true; }
        else if (y >= maxY) { y = maxY; vy = -vy; hitWall = true; }

        if (hitWall) {
            dotColor = matrix->color565(
                random(85, 255),
                random(85, 255), 
                random(85, 255)
            );
        }

        // Draw dot first, then text on top
        matrix->fillCircle((int)x, (int)y, radius, dotColor);

        // Centered "READY" — 5 chars × ~6 px = 30 px wide
        matrix->setTextColor(green);
        matrix->setCursor((64 - 5 * 6) / 2, 12);
        matrix->print("READY");
    }

}