#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "config.h"

// ── Init / loop ───────────────────────────────────────────────────────────

// Call once after Wi-Fi connects. Safe to call again on reconnect —
// internally guarded against double-init.
void api_init(Preferences &prefs);

// Call in loop() — drives ElegantOTA, WebSocket cleanup, Pomodoro tick.
void api_loop();

// Push JSON string to all connected WebSocket clients.
void api_ws_broadcast(const String &json);

// ── Global mode + brightness ──────────────────────────────────────────────
extern AppMode g_mode;
extern uint8_t g_brightness;

// ── Clock config (read by display_clock.cpp) ─────────────────────────────
struct ClockConfig {
    bool   is24h       = true;
    bool   showDate    = true;
    bool   showSeconds = false;
    String timezone    = "UTC0";
    String ntpServer   = "pool.ntp.org";
};
extern ClockConfig g_clockCfg;

// ── Spotify state (read by display_spotify.cpp) ───────────────────────────
struct SpotifyState {
    String track;
    String artist;
    bool   isPlaying = false;
    bool   hasArt    = false;
    // Art stored as base64 JPEG string (decoded on demand)
    String artBase64;
};
extern SpotifyState g_spotify;

// ── Pomodoro config + timer (read by display_pomodoro.cpp) ───────────────
struct PomodoroConfig {
    int  workMinutes        = 25;
    int  shortBreakMinutes  = 5;
    int  longBreakMinutes   = 15;
    int  sessionsBeforeLong = 4;
    bool alertOnComplete    = true;
};
extern PomodoroConfig g_pomoCfg;

enum PomodoroPhase { POMO_WORK, POMO_SHORT_BREAK, POMO_LONG_BREAK };

struct PomodoroTimer {
    PomodoroPhase phase           = POMO_WORK;
    int           secondsRemaining = 25 * 60;
    int           sessionsCompleted = 0;
    bool          running          = false;
    uint32_t      lastTick         = 0;
};
extern PomodoroTimer g_pomoTimer;
