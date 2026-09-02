// include/inputhelper.h
//
// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.h — HID report edition
//
// FIX: added displayBrightness extern — inputTask adjusts it from joystick Y
//      and displayTask applies it via matrix->setBrightness8() once per frame.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

// ── InputEventType ────────────────────────────────────────────────────────────
// Kept for API compatibility with inputApplyEvent (now a stub in HID build).
enum InputEventType : uint8_t {
    INPUT_NONE            = 0,
    INPUT_ENC_CW          = 1,
    INPUT_ENC_CCW         = 2,
    INPUT_ENC_PRESS       = 3,
    INPUT_ENC_LONG        = 4,
    INPUT_JOY_UP          = 5,
    INPUT_JOY_DOWN        = 6,
    INPUT_JOY_RIGHT       = 7,
    INPUT_JOY_LEFT        = 8,
    INPUT_JOY_CENTER      = 9,
    INPUT_JOY_PRESS       = 10,
    INPUT_JOY_LONG        = 11,
    INPUT_BTN1_PRESS      = 12,
    INPUT_BTN1_LONG       = 13,
    INPUT_BTN2_PRESS      = 14,
    INPUT_BTN2_LONG       = 15,
    INPUT_BTN3_PRESS      = 16,
    INPUT_BTN3_LONG       = 17,
    INPUT_BTN4_PRESS      = 18,
    INPUT_BTN4_LONG       = 19,
    INPUT_BTN5_PRESS      = 20,
    INPUT_BTN5_LONG       = 21,
};

// ── Shared state ──────────────────────────────────────────────────────────────

/// Shared with main.cpp, displayTask. Written atomically by inputTask (Core 1).
extern QueueHandle_t  inputQueue;
extern volatile bool  displayLocked;

/// FIX: current firmware-applied display brightness (0-255).
/// Written by inputTask (Core 1) from joystick Y axis movement.
/// Read by displayTask (Core 0) to call matrix->setBrightness8() each frame.
/// uint8_t write/read is atomic on ARM — no mutex required.
extern uint8_t displayBrightness;

// Calibrated joystick centre values (set in inputInit).
extern int joyCentreX;
extern int joyCentreY;

// ── Public API ────────────────────────────────────────────────────────────────

/// Initialise GPIO, calibrate joystick, attach encoder ISR. Call from setup().
void inputInit();

/// Spawn the FreeRTOS input task on Core 1. Call after inputInit().
void inputTaskStart();

/// Stub in HID build — HID reports are sent directly from inputTask.
void inputApplyEvent(InputEventType evt, void* matrixPtr);