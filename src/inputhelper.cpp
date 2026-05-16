// ─────────────────────────────────────────────────────────────────────────────
// inputhelper.cpp — peripherals control input driver
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
#define JOY_THRESHOLD        800    // ADC counts from centre to trigger a dir
#define JOY_CENTRE           2047   // expected ADC centre value (0-4095)
#define BRIGHTNESS_STEP      8      // brightness change per encoder click
#define BRIGHTNESS_MIN       10     // never go fully dark
#define BRIGHTNESS_MAX       255

// ─── Shared state ─────────────────────────────────────────────────────────────
QueueHandle_t        inputQueue      = nullptr;
volatile uint8_t     inputBrightness = DEFAULT_BRIGHTNESS;
volatile uint8_t     inputCurrentMode = 0;
volatile int8_t      inputMenuSelection = 0;

// ─── Encoder 1 ISR state (IRAM) ───────────────────────────────────────────────
static volatile int8_t  enc1Steps    = 0;   // accumulated steps (+CW / -CCW)
static volatile uint32_t enc1LastUs  = 0;   // debounce timestamp

static void IRAM_ATTR encoder1ISR() {
    const uint32_t now = micros();
    if (now - enc1LastUs < 2000) return;  // 2 ms hardware debounce
    enc1LastUs = now;
    // DT HIGH when CLK rises → CW; DT LOW → CCW
    if (digitalRead(PIN_ENC1_DT) == HIGH) enc1Steps++;
    else                                  enc1Steps--;
}

// ─── Encoder 2 ISR state (IRAM) ───────────────────────────────────────────────
static volatile int8_t  enc2Steps    = 0;   // accumulated steps (+CW / -CCW)
static volatile uint32_t enc2LastUs  = 0;   // debounce timestamp

static void IRAM_ATTR encoder2ISR() {
    const uint32_t now = micros();
    if (now - enc2LastUs < 2000) return;  // 2 ms hardware debounce
    enc2LastUs = now;
    // DT HIGH when CLK rises → CW; DT LOW → CCW
    if (digitalRead(PIN_ENC2_DT) == HIGH) enc2Steps++;
    else                                  enc2Steps--;
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

    // Returns typeShort or typeLong based on hold duration.
    // Returns 0xFF if no event yet.
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
        
        if (adcVal < JOY_CENTRE - JOY_THRESHOLD) dir = -1;
        else if (adcVal > JOY_CENTRE + JOY_THRESHOLD) dir = 1;
        
        // Small deadzone to prevent constant centre events
        if (dir == 0 && lastDir == 0) return 0xFF;
        
        if (dir != lastDir) {
            lastDir = dir;
            if      (dir < 0) return evtNeg;
            else if (dir > 0) return evtPos;
            else              return evtCentre;
        }
        return 0xFF;
    }
};

// ─── Task-local state ─────────────────────────────────────────────────────────
static Button  enc1Btn;      // Encoder 1 button
static Button  enc2Btn;      // Encoder 2 button
static Button  joyBtn;       // Joystick button
static Button  btn1, btn2, btn3, btn4, btn5;  // 5 push buttons
static JoyAxis joyX;
static JoyAxis joyY;

// ─── Helper: push an event into the queue without blocking ────────────────────
static inline void pushEvt(InputEventType e) {
    if (inputQueue) {
        xQueueSendToBack(inputQueue, &e, 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// inputTask — polls at INPUT_POLL_MS, flushes encoder ISR counters
// ─────────────────────────────────────────────────────────────────────────────
static void inputTask(void* /*param*/) {
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));

        // ── 1. Drain encoder 1 ISR counter ──────────────────────────────────
        const int8_t steps1 = enc1Steps;
        if (steps1 != 0) {
            enc1Steps = 0;
            const int n1 = (steps1 > 0) ? steps1 : -steps1;
            const InputEventType dir1 = (steps1 > 0) ? INPUT_ENC1_CW : INPUT_ENC1_CCW;
            for (int i = 0; i < n1; i++) pushEvt(dir1);
        }

        // ── 2. Drain encoder 2 ISR counter ──────────────────────────────────
        const int8_t steps2 = enc2Steps;
        if (steps2 != 0) {
            enc2Steps = 0;
            const int n2 = (steps2 > 0) ? steps2 : -steps2;
            const InputEventType dir2 = (steps2 > 0) ? INPUT_ENC2_CW : INPUT_ENC2_CCW;
            for (int i = 0; i < n2; i++) pushEvt(dir2);
        }

        // ── 3. Encoder 1 button ─────────────────────────────────────────────
        const uint8_t enc1Evt = enc1Btn.poll(INPUT_ENC1_PRESS, INPUT_ENC1_LONG);
        if (enc1Evt != 0xFF) pushEvt((InputEventType)enc1Evt);

        // ── 4. Encoder 2 button ─────────────────────────────────────────────
        const uint8_t enc2Evt = enc2Btn.poll(INPUT_ENC2_PRESS, INPUT_ENC2_LONG);
        if (enc2Evt != 0xFF) pushEvt((InputEventType)enc2Evt);

        // ── 5. Joystick axes (with ADC reading) ─────────────────────────────
        const int vrx = analogRead(PIN_JOY_VRX);
        const int vry = analogRead(PIN_JOY_VRY);

        // X axis: left = negative ADC, right = positive
        const uint8_t xEvt = joyX.poll(vrx,
            INPUT_JOY_LEFT, INPUT_JOY_RIGHT, INPUT_JOY_CENTER);
        if (xEvt != 0xFF) pushEvt((InputEventType)xEvt);

        // Y axis: up = negative ADC, down = positive
        const uint8_t yEvt = joyY.poll(vry,
            INPUT_JOY_UP, INPUT_JOY_DOWN, INPUT_JOY_CENTER);
        if (yEvt != 0xFF) pushEvt((InputEventType)yEvt);

        // ── 6. Joystick button ──────────────────────────────────────────────
        const uint8_t joyEvt = joyBtn.poll(INPUT_JOY_PRESS, INPUT_JOY_LONG);
        if (joyEvt != 0xFF) pushEvt((InputEventType)joyEvt);

        // ── 7. Push buttons (5x) ────────────────────────────────────────────
        const uint8_t btn1Evt = btn1.poll(INPUT_BTN1_PRESS, INPUT_BTN1_LONG);
        if (btn1Evt != 0xFF) pushEvt((InputEventType)btn1Evt);
        
        const uint8_t btn2Evt = btn2.poll(INPUT_BTN2_PRESS, INPUT_BTN2_LONG);
        if (btn2Evt != 0xFF) pushEvt((InputEventType)btn2Evt);
        
        const uint8_t btn3Evt = btn3.poll(INPUT_BTN3_PRESS, INPUT_BTN3_LONG);
        if (btn3Evt != 0xFF) pushEvt((InputEventType)btn3Evt);
        
        const uint8_t btn4Evt = btn4.poll(INPUT_BTN4_PRESS, INPUT_BTN4_LONG);
        if (btn4Evt != 0xFF) pushEvt((InputEventType)btn4Evt);
        
        const uint8_t btn5Evt = btn5.poll(INPUT_BTN5_PRESS, INPUT_BTN5_LONG);
        if (btn5Evt != 0xFF) pushEvt((InputEventType)btn5Evt);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void inputInit() {
    // ── Encoder 1 ──────────────────────────────────────────────────────────
    pinMode(PIN_ENC1_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC1_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC1_CLK), encoder1ISR, RISING);

    // ── Encoder 2 ──────────────────────────────────────────────────────────
    pinMode(PIN_ENC2_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC2_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC2_CLK), encoder2ISR, RISING);

    // ── Encoder buttons ────────────────────────────────────────────────────
    enc1Btn.init(PIN_ENC1_SW);
    enc2Btn.init(PIN_ENC2_SW);

    // ── Joystick ADC (ADC1 — safe with PSRAM / DMA; no WiFi conflict) ──────
    analogReadResolution(12);            // 0-4095
    analogSetAttenuation(ADC_11db);      // full 0-3.3 V range
    pinMode(PIN_JOY_VRX, INPUT);
    pinMode(PIN_JOY_VRY, INPUT);
    joyBtn.init(PIN_JOY_SW);
    joyX.init();
    joyY.init();

    // ── Push buttons (5x) ───────────────────────────────────────────────────
    btn1.init(PIN_BTN1);
    btn2.init(PIN_BTN2);
    btn3.init(PIN_BTN3);
    btn4.init(PIN_BTN4);
    btn5.init(PIN_BTN5);

    // ── Event queue — depth 32, one InputEventType per slot ─────────────────
    inputQueue = xQueueCreate(32, sizeof(InputEventType));
    if (!inputQueue) {
        Serial.println("FATAL: inputQueue alloc failed");
        while (true) delay(500);
    }

    Serial.println("Input modules initialized:");
    Serial.println("  - Encoder 1 (GPIO17/18/21) → brightness control");
    Serial.println("  - Encoder 2 (GPIO35/36/37) → reserved");
    Serial.println("  - Joystick (GPIO6/7/8)    → navigation");
    Serial.println("  - Buttons (GPIO9/10/11/47/48) → 5x SMD push buttons");
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTask, "inputTask", 4096, nullptr, 2, nullptr, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// inputApplyEvent — translate events into firmware actions
//
// Default mapping:
//   ENC1_CW / ENC1_CCW → brightness +/- BRIGHTNESS_STEP
//   ENC1_LONG          → brightness reset to DEFAULT_BRIGHTNESS
//   ENC1_PRESS         → toggle mode (normal/menu)
//   ENC2_CW / ENC2_CCW → menu navigation (when in menu mode)
//   ENC2_PRESS         → select menu item
//   JOY_*              → directional navigation
//   BTN*               → custom actions (extend as needed)
// ─────────────────────────────────────────────────────────────────────────────
void inputApplyEvent(InputEventType evt, void* matrixPtr) {
    MatrixPanel_I2S_DMA* mx = static_cast<MatrixPanel_I2S_DMA*>(matrixPtr);

    switch (evt) {
        // ── Encoder 1: Brightness control ───────────────────────────────────
        case INPUT_ENC1_CW: {
            int b = (int)inputBrightness + BRIGHTNESS_STEP;
            if (b > BRIGHTNESS_MAX) b = BRIGHTNESS_MAX;
            inputBrightness = (uint8_t)b;
            if (mx) mx->setBrightness8(inputBrightness);
            Serial.printf("[INPUT] Brightness ↑ %d\n", inputBrightness);
            break;
        }
        case INPUT_ENC1_CCW: {
            int b = (int)inputBrightness - BRIGHTNESS_STEP;
            if (b < BRIGHTNESS_MIN) b = BRIGHTNESS_MIN;
            inputBrightness = (uint8_t)b;
            if (mx) mx->setBrightness8(inputBrightness);
            Serial.printf("[INPUT] Brightness ↓ %d\n", inputBrightness);
            break;
        }
        case INPUT_ENC1_LONG: {
            inputBrightness = DEFAULT_BRIGHTNESS;
            if (mx) mx->setBrightness8(inputBrightness);
            Serial.printf("[INPUT] Brightness reset → %d\n", DEFAULT_BRIGHTNESS);
            break;
        }
        case INPUT_ENC1_PRESS: {
            // Toggle between normal mode and menu mode
            inputCurrentMode = (inputCurrentMode == 0) ? 1 : 0;
            Serial.printf("[INPUT] Mode → %s\n", inputCurrentMode ? "MENU" : "NORMAL");
            break;
        }

        // ── Encoder 2: Menu navigation / selection ──────────────────────────
        case INPUT_ENC2_CW: {
            if (inputCurrentMode == 1) {
                inputMenuSelection++;
                if (inputMenuSelection > 4) inputMenuSelection = 4;
                Serial.printf("[INPUT] Menu selection → %d\n", inputMenuSelection);
            } else {
                Serial.println("[INPUT] Encoder 2 CW (menu mode inactive)");
            }
            break;
        }
        case INPUT_ENC2_CCW: {
            if (inputCurrentMode == 1) {
                inputMenuSelection--;
                if (inputMenuSelection < 0) inputMenuSelection = 0;
                Serial.printf("[INPUT] Menu selection → %d\n", inputMenuSelection);
            } else {
                Serial.println("[INPUT] Encoder 2 CCW (menu mode inactive)");
            }
            break;
        }
        case INPUT_ENC2_PRESS: {
            if (inputCurrentMode == 1) {
                Serial.printf("[INPUT] Menu item %d selected\n", inputMenuSelection);
                // Execute menu action based on selection
                // Extend this as needed for your application
            } else {
                Serial.println("[INPUT] Encoder 2 press (menu mode inactive)");
            }
            break;
        }
        case INPUT_ENC2_LONG: {
            Serial.println("[INPUT] Encoder 2 long press");
            break;
        }

        // ── Joystick: Directional navigation ────────────────────────────────
        case INPUT_JOY_UP:
            Serial.println("[INPUT] Joystick ↑");
            if (inputCurrentMode == 1) {
                inputMenuSelection--;
                if (inputMenuSelection < 0) inputMenuSelection = 0;
                Serial.printf("[INPUT] Menu selection → %d\n", inputMenuSelection);
            }
            break;
        case INPUT_JOY_DOWN:
            Serial.println("[INPUT] Joystick ↓");
            if (inputCurrentMode == 1) {
                inputMenuSelection++;
                if (inputMenuSelection > 4) inputMenuSelection = 4;
                Serial.printf("[INPUT] Menu selection → %d\n", inputMenuSelection);
            }
            break;
        case INPUT_JOY_LEFT:
            Serial.println("[INPUT] Joystick ←");
            break;
        case INPUT_JOY_RIGHT:
            Serial.println("[INPUT] Joystick →");
            break;
        case INPUT_JOY_CENTER:
            Serial.println("[INPUT] Joystick CENTER");
            break;
        case INPUT_JOY_PRESS:
            Serial.println("[INPUT] Joystick press (short)");
            break;
        case INPUT_JOY_LONG:
            Serial.println("[INPUT] Joystick long press");
            break;

        // ── Push buttons (5x) ────────────────────────────────────────────────
        case INPUT_BTN1_PRESS:
            Serial.println("[INPUT] Button 1 short press");
            // Add your button 1 action here
            break;
        case INPUT_BTN1_LONG:
            Serial.println("[INPUT] Button 1 long press");
            // Add your button 1 long-press action here
            break;
        case INPUT_BTN2_PRESS:
            Serial.println("[INPUT] Button 2 short press");
            break;
        case INPUT_BTN2_LONG:
            Serial.println("[INPUT] Button 2 long press");
            break;
        case INPUT_BTN3_PRESS:
            Serial.println("[INPUT] Button 3 short press");
            break;
        case INPUT_BTN3_LONG:
            Serial.println("[INPUT] Button 3 long press");
            break;
        case INPUT_BTN4_PRESS:
            Serial.println("[INPUT] Button 4 short press");
            break;
        case INPUT_BTN4_LONG:
            Serial.println("[INPUT] Button 4 long press");
            break;
        case INPUT_BTN5_PRESS:
            Serial.println("[INPUT] Button 5 short press");
            break;
        case INPUT_BTN5_LONG:
            Serial.println("[INPUT] Button 5 long press");
            break;

        default:
            break;
    }
}