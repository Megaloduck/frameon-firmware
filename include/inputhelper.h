#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.h — KY-040 rotary encoders (×2), KY-023 analog joystick, 
//                 SMD push buttons (×5)
//
// Architecture
// ────────────
//   inputInit()     — call once from setup() on Core 1, before inputTaskStart().
//   inputTaskStart()— spawns inputTask pinned to Core 1 at priority 1.
//                     Polls the joystick ADC, debounces all buttons, drives the
//                     encoder ISRs, and pushes InputEvent items into inputQueue.
//   inputQueue      — FreeRTOS queue (depth 32). Drain from loop() on Core 1.
//   inputApplyEvent()— convenience: maps events to brightness / mode changes
//                     and writes them into the shared volatile state so
//                     displayTask (Core 0) picks them up next frame.
//
// Pin assignments (defined in frameon.h):
//   Encoder 1: CLK=GPIO17  DT=GPIO18  SW=GPIO21
//   Encoder 2: CLK=GPIO35  DT=GPIO36  SW=GPIO37
//   Joystick:  VRX=GPIO6   VRY=GPIO7  SW=GPIO8
//   Buttons:   BTN1=GPIO9  BTN2=GPIO10 BTN3=GPIO11 BTN4=GPIO47 BTN5=GPIO48
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ── Event types ───────────────────────────────────────────────────────────────
enum InputEventType : uint8_t {
    // Encoder 1 (brightness control)
    INPUT_ENC1_CW       = 0,   // clockwise step
    INPUT_ENC1_CCW      = 1,   // counter-clockwise step
    INPUT_ENC1_PRESS    = 2,   // short button press (< 600 ms)
    INPUT_ENC1_LONG     = 3,   // long press (≥ 600 ms) — resets brightness

    // Encoder 2 (reserved for menu navigation / song selection)
    INPUT_ENC2_CW       = 4,
    INPUT_ENC2_CCW      = 5,
    INPUT_ENC2_PRESS    = 6,
    INPUT_ENC2_LONG     = 7,

    // Analog joystick axis (fired once per threshold-crossing)
    INPUT_JOY_UP        = 8,
    INPUT_JOY_DOWN      = 9,
    INPUT_JOY_LEFT      = 10,
    INPUT_JOY_RIGHT     = 11,
    INPUT_JOY_CENTER    = 12,  // axis returned to dead-zone
    INPUT_JOY_PRESS     = 13,  // SW button short press
    INPUT_JOY_LONG      = 14,  // SW button long press (≥ 600 ms)

    // Push buttons (5x)
    INPUT_BTN1_PRESS    = 15,
    INPUT_BTN1_LONG     = 16,
    INPUT_BTN2_PRESS    = 17,
    INPUT_BTN2_LONG     = 18,
    INPUT_BTN3_PRESS    = 19,
    INPUT_BTN3_LONG     = 20,
    INPUT_BTN4_PRESS    = 21,
    INPUT_BTN4_LONG     = 22,
    INPUT_BTN5_PRESS    = 23,
    INPUT_BTN5_LONG     = 24,
};

// ── Queue handle — readable from main.cpp ─────────────────────────────────────
extern QueueHandle_t inputQueue;

// ── Current brightness (0-255), shared between input handler and displayTask.
// Written by inputApplyEvent(); read under swapMutex by displayTask. ──────────
extern volatile uint8_t inputBrightness;

// ── Current selected mode, shared state ──────────────────────────────────────
extern volatile uint8_t inputCurrentMode;    // 0=normal, 1=menu, etc.
extern volatile int8_t  inputMenuSelection;  // menu cursor position

// ── Initialise GPIO, ADC, and encoder ISRs. Call from setup(). ────────────────
void inputInit();

// ── Start the FreeRTOS input polling task (Core 1, priority 1). ──────────────
void inputTaskStart();

// ── Map an event to a firmware action and update shared state. ───────────────
// Call from loop() after dequeuing; pass the matrix pointer for brightness.
void inputApplyEvent(InputEventType evt, void* matrixPtr);