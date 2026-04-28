    // ─────────────────────────────────────────────────────────────────────────────
// spotifyhelper.cpp — Spotify progress bar overdraw
//
// Renders the 2-px-tall progress bar on top of each displayed frame using the
// real wall-clock elapsed time since the packet was committed, so the bar
// stays accurate between app re-syncs independent of the animation loop.
// ─────────────────────────────────────────────────────────────────────────────

#include "spotifyhelper.h"
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// Shared matrix handle — defined in main.cpp.
extern MatrixPanel_I2S_DMA* matrix;

void overdrawProgressBar(
    uint32_t songPosMs,
    uint32_t trackDurMs,
    uint8_t  barX,
    uint8_t  barY,
    uint8_t  barW,
    uint16_t barColor)
{
    if (trackDurMs == 0 || barW == 0) return;

    float p = (float)songPosMs / (float)trackDurMs;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    const int filled = (int)(barW * p + 0.5f);

    // Background: #333333 → RGB565 0x3186
    const uint16_t bgColor = 0x3186;

    for (int row = barY; row <= barY + 1; row++) {
        for (int x = 0; x < (int)barW; x++) {
            matrix->drawPixel(barX + x, row,
                              x < filled ? barColor : bgColor);
        }
    }
    // NOTE: flipDMABuffer() is called by displayTask after this returns.
}