#pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// pomodorohelper.h — Firmware-side live Pomodoro timer renderer (v1.8)
//
// Mirrors the same pattern as clockhelper.h / overdrawClock().
//
// The app sends the current timer state in the packet header at sync time:
//   • remainingSec  — seconds left in the current phase when the packet was
//                     committed (captured at ACK, same moment as commitTimeMs)
//   • phase         — 0=focus  1=shortBreak  2=longBreak
//   • isRunning     — whether the timer was running at commit
//
// overdrawPomodoro() derives the live remaining time from:
//
//     liveSec = remainingSec - (millis() - commitTimeMs) / 1000
//               (clamped to 0, only decremented while isRunning)
//
// and paints the countdown on top of the current frame, exactly like
// overdrawClock() does for the wall clock. This means:
//   • Seconds tick correctly on the panel with no baked pixels.
//   • Blink-colon uses millis() % 1000 < 500 — always accurate.
//   • The timer freezes naturally when isRunning == false.
//   • No phase transitions happen on the device — the app is the source of
//     truth for phase logic. When the user advances the phase in the app,
//     they sync again and the device gets a fresh header.
//
// Must be called only from Core 0 (displayTask), after renderFrame() and
// before flipDMABuffer(), in the same order as overdrawClock().
// ─────────────────────────────────────────────────────────────────────────────

// ── Phase constants (must match PomodoroState enum order in Dart) ─────────────
#define POMO_PHASE_FOCUS        0
#define POMO_PHASE_SHORT_BREAK  1
#define POMO_PHASE_LONG_BREAK   2

// ── Flag bits (pomodoroFlags byte in header) ──────────────────────────────────
#define POMO_FLAG_PRESENT    0x01  // pomodoro layer is active
#define POMO_FLAG_RUNNING    0x02  // timer was running at commit time
#define POMO_FLAG_SECONDS    0x04  // show seconds on the display
#define POMO_FLAG_SESSION    0x08  // show session dots
#define POMO_FLAG_BLINK      0x10  // blink colon every 500 ms

/// Render the live Pomodoro countdown on top of the current frame.
///
/// @param remainingSec   Seconds remaining in the current phase at commit.
/// @param wallMs         millis() - commitTimeMs  (elapsed since commit).
/// @param phase          POMO_PHASE_* constant for the active phase.
/// @param flags          POMO_FLAG_* bitmask (present, running, seconds, etc.)
/// @param session        Current session number (1-based, for dot display).
/// @param offX           Signed horizontal pixel nudge.
/// @param offY           Signed vertical pixel nudge.
/// @param activeColor    RGB565 colour for the timer digits (phase colour).
void overdrawPomodoro(
    uint32_t remainingSec,
    uint32_t wallMs,
    uint8_t  phase,
    uint8_t  flags,
    uint8_t  session,
    int8_t   offX,
    int8_t   offY,
    uint16_t activeColor
);