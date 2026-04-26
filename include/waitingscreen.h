    #pragma once

#include <stdint.h>

// ─────────────────────────────────────────────────────────────────────────────
// waitingscreen.h — Idle splash screen interface
//
// Include this header in any translation unit that needs to call
// showWaitingScreen().  The implementation lives in waitingscreen.cpp.
// ─────────────────────────────────────────────────────────────────────────────

/// Render the idle "FRAMEON / READY" splash to the LED matrix.
///
/// @param elapsedMs  Milliseconds since the display task started.
///                   Used to drive the 500 ms blink dot phase.
///
/// Must be called only from Core 0 (displayTask), never from Core 1,
/// so that matrix draw calls are single-threaded.
void showWaitingScreen(uint32_t elapsedMs);