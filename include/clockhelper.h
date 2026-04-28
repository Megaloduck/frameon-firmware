#pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// clockhelper.h — Firmware-side live clock renderer (v1.5)
//
// The app sends a Unix timestamp + display flags in the packet header.
// overdrawClock() derives the current wall time from:
//
//     wallSec = clockEpochSec + (millis() - commitTimeMs) / 1000
//
// and paints the clock digits on top of the current frame, exactly like
// overdrawProgressBar does for Spotify. This means:
//   • Seconds tick correctly forever — no baked pixels, no loop reset.
//   • Minutes and hours update naturally.
//   • Blink-colon uses millis() % 1000 < 500 — always accurate.
//   • No latency compensation needed — commitTimeMs is captured at ACK.
//
// Must be called only from Core 0 (displayTask).
// ─────────────────────────────────────────────────────────────────────────────

/// Render the live clock on top of the current frame.
///
/// @param epochSec    Unix time (seconds) recorded at packet commit.
/// @param wallMs      millis() - commitTimeMs  (elapsed since commit).
/// @param tzOffsetMin Signed timezone offset in minutes (from header).
/// @param flags       CLK_FLAG_* bitmask from header byte [29].
/// @param fontId      Font index (reserved; single built-in font for now).
/// @param offX        Signed horizontal pixel nudge.
/// @param offY        Signed vertical pixel nudge.
/// @param hoursCol    RGB565 colour for hour digits.
/// @param minutesCol  RGB565 colour for minute digits.
/// @param secondsCol  RGB565 colour for second digits.
/// @param colonCol    RGB565 colour for colon separator.
/// @param dateCol     RGB565 colour for date row.
/// @param ampmCol     RGB565 colour for AM/PM label.
void overdrawClock(
    uint32_t epochSec,
    uint32_t wallMs,
    int16_t  tzOffsetMin,
    uint8_t  flags,
    uint8_t  fontId,
    int8_t   offX,
    int8_t   offY,
    uint16_t hoursCol,
    uint16_t minutesCol,
    uint16_t secondsCol,
    uint16_t colonCol,
    uint16_t dateCol,
    uint16_t ampmCol
);