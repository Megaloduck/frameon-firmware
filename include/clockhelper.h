#pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// clockhelper.h — Firmware-side live clock renderer (v1.6)
//
// The app sends a Unix timestamp + display flags + layout style in the packet
// header. overdrawClock() derives the current wall time from:
//
//     wallSec = clockEpochSec + (millis() - commitTimeMs) / 1000
//
// and paints the clock on top of the current frame. Layout style is selected
// per-packet — Classic / Analog / WeekdayPrefix / Stacked / SecondsBar /
// DualTimezone — each rendered live so seconds always tick correctly.
//
// Must be called only from Core 0 (displayTask).
// ─────────────────────────────────────────────────────────────────────────────

/// Render the live clock on top of the current frame.
///
/// @param epochSec     Unix time (seconds) recorded at packet commit.
/// @param wallMs       millis() - commitTimeMs  (elapsed since commit).
/// @param tzOffsetMin  Signed timezone offset in minutes (primary zone).
/// @param tz2OffsetMin Signed offset in minutes for the second zone
///                     (used only when layoutStyle == DUAL_TIMEZONE).
/// @param flags        CLK_FLAG_* bitmask from header byte [29].
/// @param layoutStyle  CLK_LAYOUT_* from header byte [68].
/// @param analogFlags  ANALOG_* bitmask from header byte [69]
///                     (face style + show-second-hand + show-digital).
/// @param fontId       Font index for digits (0-6).
/// @param offX         Signed horizontal pixel nudge.
/// @param offY         Signed vertical pixel nudge.
/// @param hoursCol     RGB565 colour for hour digits / hour hand.
/// @param minutesCol   RGB565 colour for minute digits / minute hand.
/// @param secondsCol   RGB565 colour for second digits / second hand / bar.
/// @param colonCol     RGB565 colour for colon separator / digital colon.
/// @param dateCol      RGB565 colour for date row / analog face & markers.
/// @param ampmCol      RGB565 colour for AM/PM / weekday / zone labels.
/// @param label1       First-zone short label, 4 ASCII bytes null-padded.
/// @param label2       Second-zone short label, 4 ASCII bytes null-padded.
void overdrawClock(
    uint32_t epochSec,
    uint32_t wallMs,
    int16_t  tzOffsetMin,
    int16_t  tz2OffsetMin,
    uint8_t  flags,
    uint8_t  layoutStyle,
    uint8_t  analogFlags,
    uint8_t  fontId,
    int8_t   offX,
    int8_t   offY,
    uint16_t hoursCol,
    uint16_t minutesCol,
    uint16_t secondsCol,
    uint16_t colonCol,
    uint16_t dateCol,
    uint16_t ampmCol,
    const char* label1,
    const char* label2
);