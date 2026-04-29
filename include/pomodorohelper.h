#pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// pomodorohelper.h — Firmware-side live Pomodoro timer renderer (v1.9)
//
// v1.9 — Layout overdraw support.
//        overdrawPomodoro() now accepts a `layout` byte so the panel renders
//        the same layout the user picked in the app dropdown.
//
//        Two layouts are supported:
//          POMO_LAYOUT_SPLIT      — arc ring left, MM:SS + phase label right.
//          POMO_LAYOUT_MINIMALIST — large MM left, vertical bar + dots right.
//
// The app sends the layout value in the reserved byte [63] of the v1.8
// pomodoro descriptor (previously always 0x00). No header size change needed.
// ─────────────────────────────────────────────────────────────────────────────

// ── Phase constants (must match PomodoroState enum order in Dart) ─────────────
#define POMO_PHASE_FOCUS        0
#define POMO_PHASE_SHORT_BREAK  1
#define POMO_PHASE_LONG_BREAK   2

// ── Layout constants (must match PomodoroLayout enum order in Dart) ───────────
// Dart enum order: splitLayout=0, minimalist=1
#define POMO_LAYOUT_SPLIT       0
#define POMO_LAYOUT_MINIMALIST  1

// ── Flag bits (pomodoroFlags byte in header [52]) ─────────────────────────────
#define POMO_FLAG_PRESENT    0x01  // pomodoro layer is active
#define POMO_FLAG_RUNNING    0x02  // timer was running at commit time
#define POMO_FLAG_SECONDS    0x04  // show seconds on the display
#define POMO_FLAG_SESSION    0x08  // show session dots
#define POMO_FLAG_BLINK      0x10  // blink colon every 500 ms

/// Render the live Pomodoro countdown on top of the current frame.
///
/// @param remainingSec   Seconds remaining in the current phase at commit.
/// @param totalSec       Total seconds for the current phase (for progress).
/// @param wallMs         millis() - commitTimeMs  (elapsed since commit).
/// @param phase          POMO_PHASE_* constant for the active phase.
/// @param layout         POMO_LAYOUT_* constant (splitLayout or minimalist).
/// @param flags          POMO_FLAG_* bitmask (present, running, seconds, etc.)
/// @param session        Current session number (1-based).
/// @param sessionsTotal  Total sessions before long break (for dot count).
/// @param offX           Signed horizontal pixel nudge.
/// @param offY           Signed vertical pixel nudge.
/// @param activeColor    RGB565 colour for the active phase.
void overdrawPomodoro(
    uint32_t remainingSec,
    uint32_t totalSec,
    uint32_t wallMs,
    uint8_t  phase,
    uint8_t  layout,
    uint8_t  flags,
    uint8_t  session,
    uint8_t  sessionsTotal,
    int8_t   offX,
    int8_t   offY,
    uint16_t activeColor
);