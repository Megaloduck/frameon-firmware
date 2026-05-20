// src/inputhelper.cpp — HID report edition
//
// Instead of EVT serial lines, builds a FrameonHidReport and calls
// hidControllerSendIfChanged() each poll cycle.
// CDC Serial is now ONLY for FRM packet receive/ACK.

#include "inputhelper.h"
#include "hid_controller.h"
#include "frameon.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <string.h>

#define INPUT_POLL_MS    20
#define DEBOUNCE_MS      40
#define LONG_PRESS_MS    600
#define JOY_CALIB_READS  64
#define ENC_DEBOUNCE_US  8000

QueueHandle_t inputQueue   = nullptr;
volatile bool displayLocked = false;
int joyCentreX = 2047;
int joyCentreY = 2047;

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

    // 0=nothing  1=short-release  2=long-press-fired
    uint8_t poll() {
        const uint32_t now = millis();
        const bool raw = (digitalRead(pin) == LOW);
        if (raw != lastRaw && (now - lastChangeMs) >= DEBOUNCE_MS) {
            lastChangeMs = now; lastRaw = raw;
            if (raw)  { held = true; downMs = now; longFired = false; }
            else if (held) { held = false; return longFired ? 0 : 1; }
        }
        if (held && !longFired && (now - downMs) >= LONG_PRESS_MS) {
            longFired = true; return 2;
        }
        return 0;
    }
    bool isHeld() const { return held; }
};

static Button encBtn, joyBtn, btn1, btn2, btn3, btn4, btn5;

static void inputTask(void*) {
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));

        FrameonHidReport report{};

        // Encoder delta
        {
            const int steps = encAccum;
            if (steps != 0) {
                encAccum = 0;
                report.encDelta = (int8_t)((steps > 127) ? 127 : (steps < -127) ? -127 : steps);
            }
        }

        // Encoder button
        {
            const uint8_t r = encBtn.poll();
            if (r == 2) {
                displayLocked = !displayLocked;
                report.events |= HID_EVT_ENC_LONG;
            }
            if (encBtn.isHeld()) report.buttons |= HID_BTN_ENC_PRESS;
        }

        // Joystick — send raw ADC; app applies calibration
        report.joyX = (uint16_t)readADC(PIN_JOY_VRX);
        report.joyY = (uint16_t)readADC(PIN_JOY_VRY);

        // Joystick button
        {
            const uint8_t r = joyBtn.poll();
            if (r == 2) report.events  |= HID_EVT_JOY_LONG;
            if (joyBtn.isHeld()) report.buttons |= HID_BTN_JOY_PRESS;
        }

        // Push buttons
        auto pb = [&](Button& b, uint8_t hm, uint8_t le) {
            const uint8_t r = b.poll();
            if (r == 2) report.events  |= le;
            if (b.isHeld()) report.buttons |= hm;
        };
        pb(btn1, HID_BTN_1, HID_EVT_BTN1_LONG);
        pb(btn2, HID_BTN_2, HID_EVT_BTN2_LONG);
        pb(btn3, HID_BTN_3, HID_EVT_BTN3_LONG);
        pb(btn4, HID_BTN_4, HID_EVT_BTN4_LONG);
        pb(btn5, HID_BTN_5, HID_EVT_BTN5_LONG);

        hidControllerSendIfChanged(report);
    }
}

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