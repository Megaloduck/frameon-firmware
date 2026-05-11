#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.h — KY-040 rotary encoder, KY-023 analog joystick, TTP223B touch
//
// Architecture
// ────────────
//   inputInit()     — call once from setup() on Core 1, before inputTaskStart().
//   inputTaskStart()— spawns inputTask pinned to Core 1 at priority 1.
//                     Polls the joystick ADC, debounces all buttons, drives the
//                     encoder ISR, and pushes InputEvent items into inputQueue.
//   inputQueue      — FreeRTOS queue (depth 16). Drain from loop() on Core 1.
//   inputApplyEvent()— convenience: maps events to brightness / mode changes
//                     and writes them into the shared volatile state so
//                     displayTask (Core 0) picks them up next frame.
//
// Pin assignments (defined in frameon.h):
//   KY-040  CLK=GPIO17  DT=GPIO18  SW=GPIO21
//   KY-023  VRX=GPIO6   VRY=GPIO7  SW=GPIO8
//   TTP223B SIG=GPIO47
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ── Event types ───────────────────────────────────────────────────────────────
enum InputEventType : uint8_t {
    // Rotary encoder
    INPUT_ENC_CW        = 0,  // clockwise step
    INPUT_ENC_CCW       = 1,  // counter-clockwise step
    INPUT_ENC_PRESS     = 2,  // short button press (< 600 ms)
    INPUT_ENC_LONG      = 3,  // long press (≥ 600 ms) — resets brightness

    // Analog joystick axis (fired once per threshold-crossing)
    INPUT_JOY_UP        = 4,
    INPUT_JOY_DOWN      = 5,
    INPUT_JOY_LEFT      = 6,
    INPUT_JOY_RIGHT     = 7,
    INPUT_JOY_CENTER    = 8,  // axis returned to dead-zone
    INPUT_JOY_PRESS     = 9,  // SW button short press
    INPUT_JOY_LONG      = 10, // SW button long press (≥ 600 ms)

    // Capacitive touch
    INPUT_TOUCH_TAP     = 11, // quick touch (< 400 ms)
    INPUT_TOUCH_LONG    = 12, // sustained touch (≥ 400 ms)
};

// ── Queue handle — readable from main.cpp ─────────────────────────────────────
extern QueueHandle_t inputQueue;

// ── Current brightness (0-255), shared between input handler and displayTask.
// Written by inputApplyEvent(); read under swapMutex by displayTask. ──────────
extern volatile uint8_t inputBrightness;

// ── Initialise GPIO, ADC, and encoder ISR. Call from setup(). ────────────────
void inputInit();

// ── Start the FreeRTOS input polling task (Core 1, priority 1). ──────────────
void inputTaskStart();

// ── Map an event to a firmware action and update shared state. ───────────────
// Call from loop() after dequeuing; pass the matrix pointer for brightness.
void inputApplyEvent(InputEventType evt, void* matrixPtr);  