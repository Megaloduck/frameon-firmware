// ─────────────────────────────────────────────────────────────────────────────
// waitingscreen.cpp — Idle splash shown when no animation is loaded
//
// Displayed by displayTask (Core 0) whenever activeFrameCount == 0.
// Draws the "FRAMEON / READY" branding with a 500 ms blink dot to prove
// the firmware is alive.
// ─────────────────────────────────────────────────────────────────────────────

#include "waitingscreen.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Forward-declared in main.cpp — shared singleton matrix handle.
extern MatrixPanel_I2S_DMA* matrix;

// ─────────────────────────────────────────────────────────────────────────────
// showWaitingScreen
//
// elapsedMs — milliseconds since the display task started (used for the
//             blink phase so the dot toggles independently of wall-clock time).
// ─────────────────────────────────────────────────────────────────────────────
void showWaitingScreen(uint32_t elapsedMs) {
    matrix->clearScreen();

    // ─────────────────────────────────────
    // CONFIG
    // ─────────────────────────────────────
    const uint32_t bootDuration = 3000; // 3 seconds
    const uint16_t green = matrix->color565(33, 195, 44);
    const uint16_t white = matrix->color565(255, 255, 255);
    const uint16_t dim   = matrix->color565(15, 60, 18);

    matrix->setTextSize(1);

    // ─────────────────────────────────────
// PHASE 1: LOADING
// ─────────────────────────────────────
if (elapsedMs < bootDuration) {

    // ───── CENTERED "FRAMEON" TEXT ─────
    matrix->setTextColor(green);
    
    // "FRAMEON" is 7 characters, text size 1, each char ~6px wide = ~42px total
    int textWidth = 7 * 6;
    int textX = (64 - textWidth) / 2;  // Centered horizontally
    int textY = 8;  // Upper portion of the panel
    
    matrix->setCursor(textX, textY);
    matrix->print("FRAMEON");

    // ───── CENTERED PROGRESS BAR ─────
    float progress = (float)elapsedMs / bootDuration;

    // Progress bar dimensions
    int barWidth = 50;  // Wider for better visibility
    int barHeight = 4;
    int barX = (64 - barWidth) / 2;  // Centered horizontally
    int barY = 22;  // Lower portion of the panel

    // Draw background (border)
    matrix->drawRect(barX, barY, barWidth, barHeight, dim);

    // Fill progress (accounting for 1px border on each side)
    int fillWidth = progress * (barWidth - 2);
    if (fillWidth > 0) {
        matrix->fillRect(barX + 1, barY + 1, fillWidth, barHeight - 2, white);
    }
}

// ─────────────────────────────────────
// PHASE 2: READY + BOUNCING DOT
// ─────────────────────────────────────
else {
    // ───── FULL PANEL BOUNCING DOT WITH WALL COLOR CHANGE ─────
    static float x = 32;
    static float y = 16;
    static float vx = 2.0f;
    static float vy = 2.0f;
    static uint16_t dotColor = white;  // Start with green
    
    const int radius = 2;
    
    // Full panel boundaries (64x32)
    const int minX = radius;
    const int maxX = 64 - radius;
    const int minY = radius;
    const int maxY = 32 - radius;
    
    // Update position
    x += vx;
    y += vy;
    
    bool hitWall = false;
    
    // Bounce off walls and change color on ANY wall hit
    if (x <= minX) {
        x = minX;
        vx = -vx;
        hitWall = true;
    } else if (x >= maxX) {
        x = maxX;
        vx = -vx;
        hitWall = true;
    }
    
    if (y <= minY) {
        y = minY;
        vy = -vy;
        hitWall = true;
    } else if (y >= maxY) {
        y = maxY;
        vy = -vy;
        hitWall = true;
    }
    
    // Change color on every wall hit
    if (hitWall) {
        // Generate a bright random color
        int r = random(100, 255);
        int g = random(100, 255);
        int b = random(100, 255);
        dotColor = matrix->color565(r, g, b);
    }
    
    // Draw bouncing dot first
    matrix->fillCircle((int)x, (int)y, radius, dotColor);
    
    // ───── CENTERED "READY" TEXT (drawn ON TOP of the dot) ─────
    matrix->setTextColor(green);
    
    // Center text on 64x32 panel
    // "READY" is 5 characters, with text size 1, each char ~6px wide = ~30px total
    int textWidth = 5 * 6;  // Approximate width
    int textX = (64 - textWidth) / 2;
    int textY = 12;  // Slightly above center
    
    matrix->setCursor(textX, textY);
    matrix->print("READY");
}



    matrix->flipDMABuffer();
}