#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.h — KY-040 rotary encoder (×1), KY-023 analog joystick,
//                 SMD push buttons (×5)
//
// Architecture
// ────────────
//   inputInit()      — call once from setup() on Core 1.
//   inputTaskStart() — spawns inputTask pinned to Core 1 at priority 1.
//                      Polls joystick ADC, debounces all buttons and encoder
//                      SW, drives the encoder ISR, and pushes InputEventType
//                      items into inputQueue.
//   inputQueue       — FreeRTOS queue (depth 32).  Drain from loop() on Core 1.
//   inputApplyEvent()— maps events to firmware actions (brightness, lock) and
//                      emits structured "EVT …\n" lines on USB-CDC serial so
//                      the Frameon app can mirror hardware state in real time.
//
// Pin assignments (defined in frameon.h):
//   Encoder:   CLK=GPIO17  DT=GPIO18   SW=GPIO21
//   Joystick:  VRX=GPIO6   VRY=GPIO7   SW=GPIO8
//   Buttons:   BTN1=GPIO9  BTN2=GPIO10  BTN3=GPIO11  BTN4=GPIO47  BTN5=GPIO48
//
// EVT protocol (v2) — firmware → app over USB-CDC:
// ──────────────────────────────────────────────────────────────────────────
//   EVT PRESET +\n         encoder CW  → preset switch +
//   EVT PRESET -\n         encoder CCW → preset switch −
//   EVT PRESET CHECK\n     encoder short press → check preset number
//   EVT LOCK <0|1>\n       encoder long hold → display lock toggled
//   EVT BRIGHT <0-255>\n   joystick up/down applied brightness change
//   EVT JOY OPACITY +\n    joystick right → opacity +
//   EVT JOY OPACITY -\n    joystick left  → opacity −
//   EVT JOY CENTER\n       joystick axes returned to dead-zone
//   EVT JOY PRESS\n        joystick short tap
//   EVT JOY HOLD\n         joystick long hold → edit/save layer
//   EVT BTN 1 SYNC\n       BTN1 short → sync display
//   EVT BTN 1 RESET\n      BTN1 long  → reset to default
//   EVT BTN 2 DISCONNECT\n BTN2 short → disconnect
//   EVT BTN 2 RECONNECT\n  BTN2 long  → reconnect
//   EVT BTN 3 S\n          BTN3 short (Pomodoro: reset timer / Spotify: prev)
//   EVT BTN 3 L\n          BTN3 long  (Spotify: volume −)
//   EVT BTN 4 S\n          BTN4 short (Pomodoro: start/pause / Spotify: play/pause)
//   EVT BTN 4 L\n          BTN4 long
//   EVT BTN 5 S\n          BTN5 short (Pomodoro: next session / Spotify: next)
//   EVT BTN 5 L\n          BTN5 long  (Spotify: volume +)
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ── Event types ───────────────────────────────────────────────────────────────
enum InputEventType : uint8_t {

    // ── Rotary encoder (single) — preset navigation + display lock ────────────
    INPUT_ENC_CW    = 0,   // CW step       → Preset switch +
    INPUT_ENC_CCW   = 1,   // CCW step      → Preset switch −
    INPUT_ENC_PRESS = 2,   // Short press   → Check preset number
    INPUT_ENC_LONG  = 3,   // Long hold     → Toggle display lock

    // ── Analog joystick axes — brightness (Y) + opacity (X) ──────────────────
    INPUT_JOY_UP     = 4,  // Y neg  → Brightness +  (firmware applies)
    INPUT_JOY_DOWN   = 5,  // Y pos  → Brightness −  (firmware applies)
    INPUT_JOY_RIGHT  = 6,  // X pos  → Opacity +     (app-side)
    INPUT_JOY_LEFT   = 7,  // X neg  → Opacity −     (app-side)
    INPUT_JOY_CENTER = 8,  // axes returned to dead-zone
    INPUT_JOY_PRESS  = 9,  // SW short press → layer-specific tap
    INPUT_JOY_LONG   = 10, // SW long hold   → Edit / Save layer

    // ── Button 1 — Global navigation (top row, left) ──────────────────────────
    INPUT_BTN1_PRESS = 11, // Short → Sync display
    INPUT_BTN1_LONG  = 12, // Long  → Reset to default

    // ── Button 2 — Global navigation (top row, right) ────────────────────────
    INPUT_BTN2_PRESS = 13, // Short → Disconnect
    INPUT_BTN2_LONG  = 14, // Long  → Reconnect

    // ── Button 3 — Layer-specific (bottom row, left ←) ───────────────────────
    //   Pomodoro → Short: Reset timer
    //   Spotify  → Short: Previous song  |  Long: Volume −
    INPUT_BTN3_PRESS = 15,
    INPUT_BTN3_LONG  = 16,

    // ── Button 4 — Layer-specific (bottom row, centre ▶) ─────────────────────
    //   Pomodoro → Short: Start / Pause
    //   Spotify  → Short: Play / Pause
    INPUT_BTN4_PRESS = 17,
    INPUT_BTN4_LONG  = 18,

    // ── Button 5 — Layer-specific (bottom row, right →) ──────────────────────
    //   Pomodoro → Short: Next session
    //   Spotify  → Short: Next song  |  Long: Volume +
    INPUT_BTN5_PRESS = 19,
    INPUT_BTN5_LONG  = 20,
};

// ── Queue handle (readable from main.cpp) ─────────────────────────────────────
extern QueueHandle_t inputQueue;

// ── Shared state (written by inputApplyEvent, read by displayTask) ────────────
extern volatile uint8_t inputBrightness;  // 0–255
extern volatile bool    displayLocked;    // true when encoder long-held

// ── Public API ────────────────────────────────────────────────────────────────

/// Initialise GPIO, ADC, and encoder ISR.  Call from setup() before
/// inputTaskStart().
void inputInit();

/// Spawn the FreeRTOS input polling task (Core 1, priority 1).
void inputTaskStart();

/// Map an InputEventType to a firmware action and emit the corresponding
/// "EVT …\n" line on USB-CDC serial.  Call from loop() after dequeuing;
/// pass the matrix pointer so brightness changes can be applied immediately.
void inputApplyEvent(InputEventType evt, void* matrixPtr);