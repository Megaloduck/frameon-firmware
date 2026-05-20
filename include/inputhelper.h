#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.h  — KY-040 rotary encoder (×1), KY-023 analog joystick,
//                  SMD push buttons (×5)
//
// Controller roles
// ────────────────
//   Encoder   — Preset switcher  (CW/CCW step through saved presets)
//   Joystick  — Display modifier (layer z-order + opacity of selected layer)
//   Buttons   — Fixed controls   (sync, disconnect, reconnect, layer-specific)
//
// Joystick axes (revised)
// ───────────────────────
//   Y-axis UP   → bring selected layer forward in z-order  (EVT JOY LAYER PREV)
//   Y-axis DOWN → send selected layer backward in z-order  (EVT JOY LAYER NEXT)
//   X-axis RIGHT→ increase selected layer opacity           (EVT JOY OPACITY +)
//   X-axis LEFT → decrease selected layer opacity           (EVT JOY OPACITY -)
//
// EVT protocol (firmware → app over USB-CDC)
// ─────────────────────────────────────────────────────────────────────────────
//   EVT PRESET +\n          encoder CW   → preset switch +
//   EVT PRESET -\n          encoder CCW  → preset switch −
//   EVT PRESET CHECK\n      encoder short press
//   EVT LOCK <0|1>\n        encoder long hold → lock toggled
//   EVT JOY LAYER PREV\n    joystick up  → bringForward selected layer
//   EVT JOY LAYER NEXT\n    joystick down→ sendBackward selected layer
//   EVT JOY OPACITY +\n     joystick right → opacity +
//   EVT JOY OPACITY -\n     joystick left  → opacity −
//   EVT JOY CENTER\n        joystick back to dead-zone
//   EVT JOY PRESS\n         joystick short tap → toggle layer visibility
//   EVT JOY HOLD\n          joystick long hold → save/confirm
//   EVT BTN 1 SYNC\n        BTN1 short → sync display
//   EVT BTN 1 RESET\n       BTN1 long  → reset to default
//   EVT BTN 2 DISCONNECT\n  BTN2 short → disconnect
//   EVT BTN 2 RECONNECT\n   BTN2 long  → reconnect
//   EVT BTN 3 S/L\n         BTN3 short/long (Pomodoro / Spotify)
//   EVT BTN 4 S/L\n         BTN4 short/long
//   EVT BTN 5 S/L\n         BTN5 short/long
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

enum InputEventType : uint8_t {

    // ── Rotary encoder — preset navigation + display lock ─────────────────────
    INPUT_ENC_CW    = 0,   // CW step       → Preset +
    INPUT_ENC_CCW   = 1,   // CCW step      → Preset −
    INPUT_ENC_PRESS = 2,   // Short press   → Check preset number
    INPUT_ENC_LONG  = 3,   // Long hold     → Toggle display lock

    // ── Joystick Y-axis — layer z-order ───────────────────────────────────────
    // Physical UP   → VRY ADC falls → "layer prev" (bring forward)
    // Physical DOWN → VRY ADC rises → "layer next" (send backward)
    INPUT_JOY_LAYER_PREV = 4,  // joystick up   → bringForward selected layer
    INPUT_JOY_LAYER_NEXT = 5,  // joystick down → sendBackward selected layer

    // ── Joystick X-axis — selected layer opacity ──────────────────────────────
    INPUT_JOY_OPACITY_UP   = 6,  // joystick right → opacity +
    INPUT_JOY_OPACITY_DOWN = 7,  // joystick left  → opacity −

    // ── Joystick dead-zone / button ───────────────────────────────────────────
    INPUT_JOY_CENTER = 8,  // both axes returned to dead-zone
    INPUT_JOY_PRESS  = 9,  // SW short press → toggle layer visibility
    INPUT_JOY_LONG   = 10, // SW long hold   → save / confirm

    // ── Button 1 — Global ─────────────────────────────────────────────────────
    INPUT_BTN1_PRESS = 11, // Short → Sync display
    INPUT_BTN1_LONG  = 12, // Long  → Reset to default

    // ── Button 2 — Global ─────────────────────────────────────────────────────
    INPUT_BTN2_PRESS = 13, // Short → Disconnect
    INPUT_BTN2_LONG  = 14, // Long  → Reconnect

    // ── Button 3 — Layer-specific (← Pomodoro reset / Spotify prev+vol-) ─────
    INPUT_BTN3_PRESS = 15,
    INPUT_BTN3_LONG  = 16,

    // ── Button 4 — Layer-specific (▶ Pomodoro start-pause / Spotify play) ────
    INPUT_BTN4_PRESS = 17,
    INPUT_BTN4_LONG  = 18,

    // ── Button 5 — Layer-specific (→ Pomodoro next / Spotify next+vol+) ──────
    INPUT_BTN5_PRESS = 19,
    INPUT_BTN5_LONG  = 20,
};

// ── Queue handle ─────────────────────────────────────────────────────────────
extern QueueHandle_t inputQueue;

// ── Shared state ─────────────────────────────────────────────────────────────
extern volatile bool displayLocked;

// Calibrated joystick centre values — set in inputInit(), readable for debug.
extern int joyCentreX;
extern int joyCentreY;

// ── Public API ────────────────────────────────────────────────────────────────
void inputInit();
void inputTaskStart();
void inputApplyEvent(InputEventType evt, void* matrixPtr);