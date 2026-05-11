// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.cpp — KY-040, KY-023, TTP223B input driver
//
// Runs as a FreeRTOS task on Core 1 (same core as loop/serial) so it never
// contends with the display DMA on Core 0.
//
// Encoder rotation is ISR-driven (CLK rising edge, IRAM_ATTR) with an atomic
// step counter; everything else is polled at INPUT_POLL_MS intervals.
// ─────────────────────────────────────────────────────────────────────────────

#include "inputhelper.h"
#include "frameon.h"
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ─── Tuning constants ─────────────────────────────────────────────────────────
#define INPUT_POLL_MS        20     // joystick + button poll interval
#define DEBOUNCE_MS          40     // button debounce window
#define LONG_PRESS_MS        600    // threshold for long-press events
#define JOY_DEADZONE         200    // ±200 around centre (ADC range 0-4095)
#define JOY_THRESHOLD        800    // ADC counts from centre to trigger a dir
#define JOY_CENTRE           2047   // expected ADC centre value
#define BRIGHTNESS_STEP      8      // brightness change per encoder click
#define BRIGHTNESS_MIN       10     // never go fully dark
#define BRIGHTNESS_MAX       255

// ─── Shared state ─────────────────────────────────────────────────────────────
QueueHandle_t        inputQueue      = nullptr;
volatile uint8_t     inputBrightness = DEFAULT_BRIGHTNESS;

// ─── Encoder ISR state (IRAM) ─────────────────────────────────────────────────
static volatile int8_t  encSteps    = 0;   // accumulated steps (+CW / -CCW)
static volatile uint32_t encLastUs  = 0;   // debounce timestamp

static void IRAM_ATTR encoderISR() {
    const uint32_t now = micros();
    if (now - encLastUs < 2000) return;  // 2 ms hardware debounce
    encLastUs = now;
    // DT HIGH when CLK rises → CW; DT LOW → CCW
    if (digitalRead(PIN_ENC_DT) == HIGH) encSteps++;
    else                                  encSteps--;
}

// ─── Button helper — tracks press start time for short/long discrimination ────
struct Button {
    uint8_t  pin;
    bool     lastRaw;       // last stable raw level
    bool     pressed;       // currently held down
    uint32_t downMs;        // millis() when button first went LOW
    uint32_t lastChangeMs;  // last debounce edge

    void init(uint8_t p) {
        pin          = p;
        lastRaw      = HIGH;
        pressed      = false;
        downMs       = 0;
        lastChangeMs = 0;
        pinMode(pin, INPUT_PULLUP);
    }

    // Returns INPUT_ENC_PRESS / INPUT_ENC_LONG  (or INPUT_JOY_PRESS / _LONG)
    // based on typeShort / typeLong. Returns 0xFF if no event yet.
    uint8_t poll(uint8_t typeShort, uint8_t typeLong) {
        const uint32_t now = millis();
        const bool raw = (digitalRead(pin) == LOW);

        if (raw != lastRaw && (now - lastChangeMs) >= DEBOUNCE_MS) {
            lastChangeMs = now;
            lastRaw      = raw;
            if (raw) {
                pressed = true;
                downMs  = now;
            } else if (pressed) {
                pressed = false;
                const uint32_t held = now - downMs;
                return (held >= LONG_PRESS_MS) ? typeLong : typeShort;
            }
        }
        return 0xFF; // no event
    }
};

// ─── Joystick axis helper — fires once per threshold-crossing ─────────────────
struct JoyAxis {
    int   lastDir;  // -1, 0, +1

    void init() { lastDir = 0; }

    // dir < 0 → negative event, dir > 0 → positive event, dir == 0 → centre.
    // Returns the event type or 0xFF if no change.
    uint8_t poll(int adcVal, uint8_t evtNeg, uint8_t evtPos, uint8_t evtCentre) {
        int dir = 0;
        if      (adcVal < JOY_CENTRE - JOY_THRESHOLD) dir = -1;
        else if (adcVal > JOY_CENTRE + JOY_THRESHOLD) dir =  1;

        if (dir != lastDir) {
            lastDir = dir;
            if      (dir < 0) return evtNeg;
            else if (dir > 0) return evtPos;
            else              return evtCentre;
        }
        return 0xFF;
    }
};

// ─── Touch helper — distinguishes tap from long touch ─────────────────────────
struct Touch {
    bool     lastRaw;
    bool     active;
    uint32_t downMs;
    uint32_t lastChangeMs;

    void init() {
        lastRaw      = false;
        active       = false;
        downMs       = 0;
        lastChangeMs = 0;
        pinMode(PIN_TOUCH, INPUT);
    }

    // Returns INPUT_TOUCH_TAP, INPUT_TOUCH_LONG, or 0xFF.
    uint8_t poll() {
        const uint32_t now = millis();
        const bool raw = (digitalRead(PIN_TOUCH) == HIGH);

        if (raw != lastRaw && (now - lastChangeMs) >= DEBOUNCE_MS) {
            lastChangeMs = now;
            lastRaw      = raw;
            if (raw) {
                active = true;
                downMs = now;
            } else if (active) {
                active = false;
                const uint32_t held = now - downMs;
                return (held >= 400) ? (uint8_t)INPUT_TOUCH_LONG
                                     : (uint8_t)INPUT_TOUCH_TAP;
            }
        }
        return 0xFF;
    }
};

// ─── Task-local state ─────────────────────────────────────────────────────────
static Button  encBtn;
static Button  joyBtn;
static JoyAxis joyX;
static JoyAxis joyY;
static Touch   touchSensor;

// ─── Helper: push an event into the queue without blocking ────────────────────
static inline void pushEvt(InputEventType e) {
    xQueueSendToBack(inputQueue, &e, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// inputTask — polls at INPUT_POLL_MS, flushes encoder ISR counter
// ─────────────────────────────────────────────────────────────────────────────
static void inputTask(void* /*param*/) {
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));

        // ── 1. Drain encoder ISR counter ──────────────────────────────────
        const int8_t steps = encSteps;
        if (steps != 0) {
            encSteps = 0;
            const int n = (steps > 0) ? steps : -steps;
            const InputEventType dir = (steps > 0) ? INPUT_ENC_CW : INPUT_ENC_CCW;
            for (int i = 0; i < n; i++) pushEvt(dir);
        }

        // ── 2. Encoder button ─────────────────────────────────────────────
        const uint8_t encEvt = encBtn.poll(INPUT_ENC_PRESS, INPUT_ENC_LONG);
        if (encEvt != 0xFF) pushEvt((InputEventType)encEvt);

        // ── 3. Joystick axes ──────────────────────────────────────────────
        const int vrx = analogRead(PIN_JOY_VRX);
        const int vry = analogRead(PIN_JOY_VRY);

        // X axis: left = negative ADC, right = positive
        const uint8_t xEvt = joyX.poll(vrx,
            INPUT_JOY_LEFT, INPUT_JOY_RIGHT, INPUT_JOY_CENTER);
        if (xEvt != 0xFF) pushEvt((InputEventType)xEvt);

        // Y axis: up = negative ADC (joystick pulled toward user), down = positive
        const uint8_t yEvt = joyY.poll(vry,
            INPUT_JOY_UP, INPUT_JOY_DOWN, INPUT_JOY_CENTER);
        if (yEvt != 0xFF) pushEvt((InputEventType)yEvt);

        // ── 4. Joystick button ────────────────────────────────────────────
        const uint8_t joyEvt = joyBtn.poll(INPUT_JOY_PRESS, INPUT_JOY_LONG);
        if (joyEvt != 0xFF) pushEvt((InputEventType)joyEvt);

        // ── 5. Touch sensor ───────────────────────────────────────────────
        const uint8_t tEvt = touchSensor.poll();
        if (tEvt != 0xFF) pushEvt((InputEventType)tEvt);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void inputInit() {
    // Encoder CLK + DT
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, RISING);

    // Encoder + joystick buttons
    encBtn.init(PIN_ENC_SW);
    joyBtn.init(PIN_JOY_SW);

    // Joystick ADC (ADC1 — safe with PSRAM / DMA; no WiFi conflict)
    analogReadResolution(12);            // 0-4095
    analogSetAttenuation(ADC_11db);      // full 0-3.3 V range
    pinMode(PIN_JOY_VRX, INPUT);
    pinMode(PIN_JOY_VRY, INPUT);
    joyX.init();
    joyY.init();

    // Touch sensor
    touchSensor.init();

    // Event queue — depth 16, one InputEventType per slot
    inputQueue = xQueueCreate(16, sizeof(InputEventType));
    if (!inputQueue) {
        Serial.println("FATAL: inputQueue alloc failed");
        while (true) delay(500);
    }

    Serial.println("Input modules OK  (KY-040 / KY-023 / TTP223B).");
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 1, nullptr, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// inputApplyEvent — translate events into firmware actions
//
// Default mapping:
//   ENC_CW / ENC_CCW  → brightness +/- BRIGHTNESS_STEP
//   ENC_LONG          → brightness reset to DEFAULT_BRIGHTNESS
//   ENC_PRESS         → reserved (log only — extend as needed)
//   TOUCH_TAP         → Serial log "touch tap"  (hook for custom action)
//   TOUCH_LONG        → Serial log "touch long" (hook for custom action)
//   JOY_*             → Serial log direction    (hook for menu navigation)
// ─────────────────────────────────────────────────────────────────────────────
void inputApplyEvent(InputEventType evt, void* matrixPtr) {
    MatrixPanel_I2S_DMA* mx = static_cast<MatrixPanel_I2S_DMA*>(matrixPtr);

    switch (evt) {
        // ── Brightness via encoder ─────────────────────────────────────────
        case INPUT_ENC_CW: {
            int b = (int)inputBrightness + BRIGHTNESS_STEP;
            if (b > BRIGHTNESS_MAX) b = BRIGHTNESS_MAX;
            inputBrightness = (uint8_t)b;
            mx->setBrightness8(inputBrightness);
            Serial.printf("[INPUT] Brightness → %d\n", inputBrightness);
            break;
        }
        case INPUT_ENC_CCW: {
            int b = (int)inputBrightness - BRIGHTNESS_STEP;
            if (b < BRIGHTNESS_MIN) b = BRIGHTNESS_MIN;
            inputBrightness = (uint8_t)b;
            mx->setBrightness8(inputBrightness);
            Serial.printf("[INPUT] Brightness → %d\n", inputBrightness);
            break;
        }
        case INPUT_ENC_LONG: {
            inputBrightness = DEFAULT_BRIGHTNESS;
            mx->setBrightness8(inputBrightness);
            Serial.println("[INPUT] Brightness reset to default.");
            break;
        }
        case INPUT_ENC_PRESS:
            Serial.println("[INPUT] Encoder press.");
            break;

        // ── Touch ──────────────────────────────────────────────────────────
        case INPUT_TOUCH_TAP:
            Serial.println("[INPUT] Touch tap.");
            break;
        case INPUT_TOUCH_LONG:
            Serial.println("[INPUT] Touch long press.");
            break;

        // ── Joystick ───────────────────────────────────────────────────────
        case INPUT_JOY_UP:     Serial.println("[INPUT] Joy UP.");     break;
        case INPUT_JOY_DOWN:   Serial.println("[INPUT] Joy DOWN.");   break;
        case INPUT_JOY_LEFT:   Serial.println("[INPUT] Joy LEFT.");   break;
        case INPUT_JOY_RIGHT:  Serial.println("[INPUT] Joy RIGHT.");  break;
        case INPUT_JOY_CENTER: Serial.println("[INPUT] Joy CENTER."); break;
        case INPUT_JOY_PRESS:  Serial.println("[INPUT] Joy press.");  break;
        case INPUT_JOY_LONG:   Serial.println("[INPUT] Joy long.");   break;

        default: break;
    }
}