#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// debug_log.h — conditional Serial logging
//
// At 921600 baud the receive path cannot afford to share the CPU with
// Serial.println() / Serial.printf(). These macros compile to no-ops in
// release builds, eliminating the 1-3 ms gaps during which the RX FIFO
// would otherwise overflow.
//
// Enable for bring-up: -DFRAMEON_DEBUG_RX=1 in platformio.ini build_flags.
// ─────────────────────────────────────────────────────────────────────────────

#ifndef FRAMEON_DEBUG_RX
#define FRAMEON_DEBUG_RX 0
#endif

#if FRAMEON_DEBUG_RX
  #define DLOG(...)   Serial.print(__VA_ARGS__)
  #define DLOGLN(...) Serial.println(__VA_ARGS__)
  #define DLOGF(...)  Serial.printf(__VA_ARGS__)
#else
  #define DLOG(...)   ((void)0)
  #define DLOGLN(...) ((void)0)
  #define DLOGF(...)  ((void)0)
#endif

// Boot-time and error logging — always on, but only fires outside the
// hot receive path so it cannot starve RX.
#define BOOTLOG(...)   Serial.print(__VA_ARGS__)
#define BOOTLOGLN(...) Serial.println(__VA_ARGS__)
#define BOOTLOGF(...)  Serial.printf(__VA_ARGS__)