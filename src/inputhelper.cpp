// src/inputhelper.cpp — HID report edition
//
// Builds a FrameonHidReport each poll cycle and calls hidControllerSendIfChanged().
// CDC Serial is now ONLY for FRM packet receive/ACK.
//
// ── FIX SUMMARY ───────────────────────────────────────────────────────────────
//
//  Bug 1 (FIXED): report.joyY was being sent as raw ADC uint16, but Dart expects
//    uint8_t brightness at that byte offset. Result: Dart read joyY_lo as
//    brightness (wrong) and joyY_hi as the buttons field (garbage — ~0x07 when
//    joystick centred → appeared as encPressed|joyPressed|btn1Pressed always held).
//    FIX: removed report.joyY. Added report.brightness = displayBrightness.
//         displayBrightness is adjusted each poll from the joystick Y axis,
//         clamped to 0-255, and applied to the matrix via setBrightness8().
//
//  Bug 2 (FIXED): Button::poll() returning 1 (short release) was silently
//    discarded by the pb() lambda — taps was never set in any report.
//    Result: btn1Tap, btn2Tap, joyTap, encTap always false → "Sync display"
//    (BTN1 short press) and other tap actions never fired.
//    FIX: pb() now takes a third mask argument (ts) for the tap bit and sets
//    report.taps |= ts when r == 1 (short release).
//    Same fix applied to the encoder and joystick buttons.
//
// ─────────────────────────────────────────────────────────────────────────────

#include "inputhelper.h"
#include "hid_controller.h"
#include "frameon.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

// ── Timing constants ───────────────────────────────────────────────────────────
#define INPUT_POLL_MS    20     // HID report rate
#define DEBOUNCE_MS      40
#define LONG_PRESS_MS    600
#define JOY_CALIB_READS  64
#define ENC_DEBOUNCE_US  8000

// ── Joystick Y → brightness constants ─────────────────────────────────────────
// Dead zone around centre: raw ADC units (0-4095).
#define JOY_Y_DEAD       400
// Brightness step per poll tick when joystick is past dead zone.
// At INPUT_POLL_MS=20ms and step=2: full sweep (0→255) in ~2.6 s.
#define JOY_Y_STEP       2

// ── Shared state ───────────────────────────────────────────────────────────────

QueueHandle_t inputQueue    = nullptr;
volatile bool displayLocked = false;
int           joyCentreX    = 2047;
int           joyCentreY    = 2047;

// FIX: displayBrightness replaces joyY in the HID report.
// Updated each poll by joystick Y; read by displayTask to call setBrightness8().
uint8_t displayBrightness = DEFAULT_BRIGHTNESS;

// ── Internal helpers ───────────────────────────────────────────────────────────

static int readADC(uint8_t pin) {
    int32_t s = 0;
    for (int i = 0; i < 4; i++) s += analogRead(pin);
    return s / 4;
}

static volatile int8_t   encAccum  = 0;
static volatile uint32_t encLastUs = 0;

static void IRAM_ATTR encoderISR() {
    const uint32_t now = micros();
    if (now - encLastUs < ENC_DEBOUNCE_US) return;
    encLastUs = now;
    if (digitalRead(PIN_ENC_DT) == HIGH) encAccum++;
    else                                  encAccum--;
}

struct Button {
    uint8_t  pin;
    bool     lastRaw, held, longFired;
    uint32_t downMs, lastChangeMs;

    void init(uint8_t p) {
        pin = p; lastRaw = HIGH; held = longFired = false;
        downMs = lastChangeMs = 0;
        pinMode(pin, INPUT_PULLUP);
    }

    /// Returns: 0 = nothing, 1 = short release (tap), 2 = long-press fired.
    uint8_t poll() {
        const uint32_t now = millis();
        const bool raw = (digitalRead(pin) == LOW);
        if (raw != lastRaw && (now - lastChangeMs) >= DEBOUNCE_MS) {
            lastChangeMs = now; lastRaw = raw;
            if (raw)  { held = true; downMs = now; longFired = false; }
            else if (held) {
                held = false;
                return longFired ? 0 : 1;  // 1 = short tap on release
            }
        }
        if (held && !longFired && (now - downMs) >= LONG_PRESS_MS) {
            longFired = true; return 2;
        }
        return 0;
    }
    bool isHeld() const { return held; }
};

static Button encBtn, joyBtn, btn1, btn2, btn3, btn4, btn5;

// ── Input task ─────────────────────────────────────────────────────────────────

static void inputTask(void*) {
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));

        FrameonHidReport report{};

        // ── Encoder delta ─────────────────────────────────────────────────────
        {
            const int steps = encAccum;
            if (steps != 0) {
                encAccum    = 0;
                report.encDelta = (int8_t)((steps >  127) ?  127 :
                                           (steps < -127) ? -127 : steps);
            }
        }

        // ── Encoder button ────────────────────────────────────────────────────
        {
            const uint8_t r = encBtn.poll();
            if (r == 1) report.taps   |= HID_TAP_ENC;    // FIX: tap captured
            if (r == 2) {
                displayLocked = !displayLocked;
                report.events |= HID_EVT_ENC_LONG;
            }
            if (encBtn.isHeld()) report.buttons |= HID_BTN_ENC_PRESS;
        }

        // ── Joystick X — raw ADC for app-side opacity control ─────────────────
        report.joyX = (uint16_t)readADC(PIN_JOY_VRX);

        // ── Joystick Y — firmware-applied brightness ───────────────────────────
        // FIX: joyY raw ADC no longer goes in the report (was causing the
        // HID layout mismatch). Instead the firmware converts it to brightness
        // and reports the resulting value in the brightness field.
        {
            const int raw   = readADC(PIN_JOY_VRY);
            const int delta = raw - joyCentreY;
            if (delta > JOY_Y_DEAD) {
                // Joystick pushed up → brightness increases
                const int next = (int)displayBrightness + JOY_Y_STEP;
                displayBrightness = (uint8_t)(next > 255 ? 255 : next);
            } else if (delta < -JOY_Y_DEAD) {
                // Joystick pushed down → brightness decreases
                const int next = (int)displayBrightness - JOY_Y_STEP;
                displayBrightness = (uint8_t)(next < 0 ? 0 : next);
            }
            // displayTask on Core 0 reads displayBrightness and calls
            // matrix->setBrightness8() — keeping matrix calls single-core.
        }
        report.brightness = displayBrightness;

        // ── Joystick button ───────────────────────────────────────────────────
        {
            const uint8_t r = joyBtn.poll();
            if (r == 1) report.taps   |= HID_TAP_JOY;   // FIX: tap captured
            if (r == 2) report.events |= HID_EVT_JOY_LONG;
            if (joyBtn.isHeld()) report.buttons |= HID_BTN_JOY_PRESS;
        }

        // ── Push buttons (BTN1-5) ─────────────────────────────────────────────
        // FIX: pb() now takes a tap-mask argument (ts) so short releases (r==1)
        // set report.taps. Previously r==1 was silently discarded, so every
        // btn*Tap accessor in Dart returned false and "Sync display" (BTN1 short
        // tap) never triggered sendToDevice().
        auto pb = [&](Button& b, uint8_t held_mask, uint8_t evt_mask, uint8_t tap_mask) {
            const uint8_t r = b.poll();
            if (r == 1) report.taps   |= tap_mask;   // FIX: short tap captured
            if (r == 2) report.events |= evt_mask;   // long press (unchanged)
            if (b.isHeld()) report.buttons |= held_mask;
        };

        pb(btn1, HID_BTN_1, HID_EVT_BTN1_LONG, HID_TAP_BTN1);
        pb(btn2, HID_BTN_2, HID_EVT_BTN2_LONG, HID_TAP_BTN2);
        pb(btn3, HID_BTN_3, HID_EVT_BTN3_LONG, HID_TAP_BTN3);
        pb(btn4, HID_BTN_4, HID_EVT_BTN4_LONG, HID_TAP_BTN4);
        pb(btn5, HID_BTN_5, HID_EVT_BTN5_LONG, HID_TAP_BTN5);

        hidControllerSendIfChanged(report);
    }
}

// ── Public functions ───────────────────────────────────────────────────────────

void inputInit() {
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, RISING);
    encBtn.init(PIN_ENC_SW);

    analogReadResolution(12);
    analogSetPinAttenuation(PIN_JOY_VRX, ADC_11db);
    analogSetPinAttenuation(PIN_JOY_VRY, ADC_11db);

    {
        int32_t sx = 0, sy = 0;
        for (int i = 0; i < JOY_CALIB_READS; i++) {
            sx += analogRead(PIN_JOY_VRX);
            sy += analogRead(PIN_JOY_VRY);
            delay(1);
        }
        joyCentreX = sx / JOY_CALIB_READS;
        joyCentreY = sy / JOY_CALIB_READS;
    }
    Serial.printf("[INPUT] Joystick centre X=%d Y=%d\n", joyCentreX, joyCentreY);

    joyBtn.init(PIN_JOY_SW);
    btn1.init(PIN_BTN1); btn2.init(PIN_BTN2); btn3.init(PIN_BTN3);
    btn4.init(PIN_BTN4); btn5.init(PIN_BTN5);

    inputQueue = xQueueCreate(8, sizeof(InputEventType));
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTask, "inputTask", 4096, nullptr, 1, nullptr, 1);
}

void inputApplyEvent(InputEventType evt, void*) { (void)evt; } // stub — HID sends directly