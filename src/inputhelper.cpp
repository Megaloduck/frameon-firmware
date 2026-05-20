// src/inputhelper.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// Physical input handler — KY-040 rotary encoder (×1), KY-023 analog
// joystick, SMD push buttons (×5).
//
// Controller action mapping (matches Frameon_Controller_actions.xlsx):
//
//   Encoder CW          → Preset switch +
//   Encoder CCW         → Preset switch −
//   Encoder short press → Check preset number
//   Encoder long hold   → Lock / Unlock display   [firmware state + EVT]
//
//   Joystick up         → Brightness +             [firmware applies + EVT]
//   Joystick down       → Brightness −             [firmware applies + EVT]
//   Joystick right      → Opacity +                [EVT → app handles]
//   Joystick left       → Opacity −                [EVT → app handles]
//   Joystick short tap  → Layer-specific tap       [EVT → app handles]
//   Joystick long hold  → Edit / Save layer        [EVT → app handles]
//
//   BTN1 short → Sync display                      [EVT → app handles]
//   BTN1 long  → Reset to default                  [EVT → app handles]
//   BTN2 short → Disconnect                        [EVT → app handles]
//   BTN2 long  → Reconnect                         [EVT → app handles]
//   BTN3 short → Pomodoro: Reset timer  /  Spotify: Previous song
//   BTN3 long  →                        /  Spotify: Volume −
//   BTN4 short → Pomodoro: Start/Pause  /  Spotify: Play / Pause
//   BTN5 short → Pomodoro: Next session /  Spotify: Next song
//   BTN5 long  →                        /  Spotify: Volume +
//
// Architecture
// ────────────
//   Encoder rotation is ISR-driven (CLK rising edge, IRAM_ATTR) with an
//   atomic step counter.  Everything else is polled every INPUT_POLL_MS ms
//   by inputTask on Core 1.
//
//   All state changes emit a structured "EVT …\n" line over USB-CDC serial
//   (followed by Serial.flush()) so the Frameon app's background reader can
//   mirror the device state without any additional polling.
// ─────────────────────────────────────────────────────────────────────────────

#include "inputhelper.h"
#include "frameon.h"
#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// ─── Tuning constants ─────────────────────────────────────────────────────────
#define INPUT_POLL_MS    20    // joystick + button poll interval (ms)
#define DEBOUNCE_MS      40    // button / encoder-SW debounce window (ms)
#define LONG_PRESS_MS    600   // short-vs-long threshold (ms)
#define JOY_THRESHOLD    800   // ADC counts from centre to fire a direction
#define JOY_CENTRE       2047  // expected ADC midpoint (12-bit, 0-4095)
#define BRIGHTNESS_STEP  8     // brightness delta per joystick tick
#define BRIGHTNESS_MIN   10    // never go fully dark
#define BRIGHTNESS_MAX   255

// ─── Shared state ─────────────────────────────────────────────────────────────
QueueHandle_t      inputQueue     = nullptr;
volatile uint8_t   inputBrightness = DEFAULT_BRIGHTNESS;
volatile bool      displayLocked   = false;

// ─── Encoder ISR state (IRAM) ─────────────────────────────────────────────────
static volatile int8_t   encSteps  = 0;    // accumulated steps (+CW / −CCW)
static volatile uint32_t encLastUs = 0;    // debounce timestamp

static void IRAM_ATTR encoderISR() {
    const uint32_t now = micros();
    if (now - encLastUs < 2000) return;    // 2 µs hardware debounce
    encLastUs = now;
    if (digitalRead(PIN_ENC_DT) == HIGH) encSteps++;
    else                                  encSteps--;
}

// ─── Button helper ────────────────────────────────────────────────────────────
// Tracks press start time; returns typeShort / typeLong on release, 0xFF
// while no event is ready.
struct Button {
    uint8_t  pin;
    bool     lastRaw;
    bool     pressed;
    uint32_t downMs;
    uint32_t lastChangeMs;

    void init(uint8_t p) {
        pin          = p;
        lastRaw      = HIGH;
        pressed      = false;
        downMs       = 0;
        lastChangeMs = 0;
        pinMode(pin, INPUT_PULLUP);
    }

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
                return ((now - downMs) >= LONG_PRESS_MS) ? typeLong : typeShort;
            }
        }
        return 0xFF;
    }
};

// ─── Joystick axis helper ─────────────────────────────────────────────────────
// Fires once per threshold-crossing; has a small dead-zone at centre to
// prevent jitter.
struct JoyAxis {
    int lastDir;   // −1, 0, +1

    void init() { lastDir = 0; }

    uint8_t poll(int adcVal, uint8_t evtNeg, uint8_t evtPos, uint8_t evtCentre) {
        int dir = 0;
        if (adcVal < JOY_CENTRE - JOY_THRESHOLD) dir = -1;
        else if (adcVal > JOY_CENTRE + JOY_THRESHOLD) dir =  1;

        if (dir == 0 && lastDir == 0) return 0xFF;   // still at centre
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
static Button  encBtn;                          // encoder push button
static Button  joyBtn;                          // joystick push button
static Button  btn1, btn2, btn3, btn4, btn5;    // 5 push buttons
static JoyAxis joyX, joyY;

static inline void pushEvt(InputEventType e) {
    if (inputQueue) xQueueSendToBack(inputQueue, &e, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// inputTask — Core 1, priority 1
// ─────────────────────────────────────────────────────────────────────────────
static void inputTask(void* /*param*/) {
    TickType_t wake = xTaskGetTickCount();

    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(INPUT_POLL_MS));

        // ── 1. Encoder rotation (drain ISR counter) ──────────────────────────
        const int8_t steps = encSteps;
        if (steps != 0) {
            encSteps = 0;
            const int n     = (steps > 0) ? steps : -steps;
            const InputEventType dir = (steps > 0) ? INPUT_ENC_CW : INPUT_ENC_CCW;
            for (int i = 0; i < n; i++) pushEvt(dir);
        }

        // ── 2. Encoder button ────────────────────────────────────────────────
        const uint8_t encEvt = encBtn.poll(INPUT_ENC_PRESS, INPUT_ENC_LONG);
        if (encEvt != 0xFF) pushEvt((InputEventType)encEvt);

        // ── 3. Joystick axes ─────────────────────────────────────────────────
        //   VRX: right (+) = opacity+, left (−) = opacity−
        //   VRY: up  (−)   = brightness+, down (+) = brightness−
        const int vrx = analogRead(PIN_JOY_VRX);
        const int vry = analogRead(PIN_JOY_VRY);

        const uint8_t xEvt = joyX.poll(vrx,
            INPUT_JOY_LEFT, INPUT_JOY_RIGHT, INPUT_JOY_CENTER);
        if (xEvt != 0xFF) pushEvt((InputEventType)xEvt);

        const uint8_t yEvt = joyY.poll(vry,
            INPUT_JOY_UP, INPUT_JOY_DOWN, INPUT_JOY_CENTER);
        if (yEvt != 0xFF) pushEvt((InputEventType)yEvt);

        // ── 4. Joystick button ───────────────────────────────────────────────
        const uint8_t joyEvt = joyBtn.poll(INPUT_JOY_PRESS, INPUT_JOY_LONG);
        if (joyEvt != 0xFF) pushEvt((InputEventType)joyEvt);

        // ── 5. Push buttons ──────────────────────────────────────────────────
        const uint8_t b1e = btn1.poll(INPUT_BTN1_PRESS, INPUT_BTN1_LONG); if (b1e != 0xFF) pushEvt((InputEventType)b1e);
        const uint8_t b2e = btn2.poll(INPUT_BTN2_PRESS, INPUT_BTN2_LONG); if (b2e != 0xFF) pushEvt((InputEventType)b2e);
        const uint8_t b3e = btn3.poll(INPUT_BTN3_PRESS, INPUT_BTN3_LONG); if (b3e != 0xFF) pushEvt((InputEventType)b3e);
        const uint8_t b4e = btn4.poll(INPUT_BTN4_PRESS, INPUT_BTN4_LONG); if (b4e != 0xFF) pushEvt((InputEventType)b4e);
        const uint8_t b5e = btn5.poll(INPUT_BTN5_PRESS, INPUT_BTN5_LONG); if (b5e != 0xFF) pushEvt((InputEventType)b5e);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Public API
// ─────────────────────────────────────────────────────────────────────────────

void inputInit() {
    // ── Encoder ──────────────────────────────────────────────────────────────
    pinMode(PIN_ENC_CLK, INPUT_PULLUP);
    pinMode(PIN_ENC_DT,  INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_CLK), encoderISR, RISING);
    encBtn.init(PIN_ENC_SW);

    // ── Joystick ─────────────────────────────────────────────────────────────
    analogReadResolution(12);           // 0-4095
    analogSetAttenuation(ADC_11db);     // full 0-3.3 V range
    pinMode(PIN_JOY_VRX, INPUT);
    pinMode(PIN_JOY_VRY, INPUT);
    joyBtn.init(PIN_JOY_SW);
    joyX.init();
    joyY.init();

    // ── Push buttons ─────────────────────────────────────────────────────────
    btn1.init(PIN_BTN1);
    btn2.init(PIN_BTN2);
    btn3.init(PIN_BTN3);
    btn4.init(PIN_BTN4);
    btn5.init(PIN_BTN5);

    // ── Event queue ──────────────────────────────────────────────────────────
    inputQueue = xQueueCreate(32, sizeof(InputEventType));
    if (!inputQueue) {
        Serial.println("FATAL: inputQueue alloc failed");
        while (true) delay(500);
    }
}

void inputTaskStart() {
    xTaskCreatePinnedToCore(inputTask, "inputTask", 4096, nullptr, 1, nullptr, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// inputApplyEvent
//
// Translates raw InputEventType values into:
//   (a) firmware-local state changes (brightness, lock), and
//   (b) structured "EVT …\n" lines emitted over USB-CDC serial so the
//       Frameon app can react without any round-trip polling.
//
// Serial.flush() is called after every EVT line.  Without it, TinyUSB
// (ESP32-S3 native CDC) buffers outgoing bytes until a 64-byte USB packet
// fills up, so a short line would never arrive at the app.
//
// Rule: Serial.printf (debug / EVT) must appear BEFORE Serial.write
// (binary ACK/NAK/ERR) on any given transaction — see firmware note in
// frameon.h — but since inputApplyEvent never sends binary response bytes,
// this ordering is automatically satisfied here.
// ─────────────────────────────────────────────────────────────────────────────
void inputApplyEvent(InputEventType evt, void* matrixPtr) {
    MatrixPanel_I2S_DMA* mx = static_cast<MatrixPanel_I2S_DMA*>(matrixPtr);

    switch (evt) {

        // ── Encoder: Preset navigation ────────────────────────────────────────
        case INPUT_ENC_CW:
            // App receives EVT and switches to the next preset, then sends
            // a new FRM packet with the updated animation.
            Serial.printf("EVT PRESET +\n");
            Serial.flush();
            break;

        case INPUT_ENC_CCW:
            Serial.printf("EVT PRESET -\n");
            Serial.flush();
            break;

        case INPUT_ENC_PRESS:
            // App displays or logs the current preset number.
            Serial.printf("EVT PRESET CHECK\n");
            Serial.flush();
            break;

        case INPUT_ENC_LONG:
            // Toggle display lock on firmware side; notify the app.
            // When locked the app stops sending new FRM packets.
            displayLocked = !displayLocked;
            Serial.printf("EVT LOCK %d\n", displayLocked ? 1 : 0);
            Serial.flush();
            break;

        // ── Joystick Y-axis: Brightness (firmware applies immediately) ─────────
        case INPUT_JOY_UP: {
            int b = (int)inputBrightness + BRIGHTNESS_STEP;
            if (b > BRIGHTNESS_MAX) b = BRIGHTNESS_MAX;
            inputBrightness = (uint8_t)b;
            if (mx) mx->setBrightness8(inputBrightness);
            Serial.printf("EVT BRIGHT %d\n", inputBrightness);
            Serial.flush();
            break;
        }

        case INPUT_JOY_DOWN: {
            int b = (int)inputBrightness - BRIGHTNESS_STEP;
            if (b < BRIGHTNESS_MIN) b = BRIGHTNESS_MIN;
            inputBrightness = (uint8_t)b;
            if (mx) mx->setBrightness8(inputBrightness);
            Serial.printf("EVT BRIGHT %d\n", inputBrightness);
            Serial.flush();
            break;
        }

        // ── Joystick X-axis: Opacity (app-side layer property) ────────────────
        case INPUT_JOY_RIGHT:
            Serial.printf("EVT JOY OPACITY +\n");
            Serial.flush();
            break;

        case INPUT_JOY_LEFT:
            Serial.printf("EVT JOY OPACITY -\n");
            Serial.flush();
            break;

        case INPUT_JOY_CENTER:
            Serial.printf("EVT JOY CENTER\n");
            Serial.flush();
            break;

        // ── Joystick button ───────────────────────────────────────────────────
        case INPUT_JOY_PRESS:
            // Context-sensitive: Spotify = refresh now playing,
            //                    Slot machine = spin roulette, etc.
            Serial.printf("EVT JOY PRESS\n");
            Serial.flush();
            break;

        case INPUT_JOY_LONG:
            // All layers: Edit / Save layer
            Serial.printf("EVT JOY HOLD\n");
            Serial.flush();
            break;

        // ── BTN1: Global — Sync / Reset ───────────────────────────────────────
        case INPUT_BTN1_PRESS:
            // App re-sends the current FRM packet to the device.
            Serial.printf("EVT BTN 1 SYNC\n");
            Serial.flush();
            break;

        case INPUT_BTN1_LONG:
            // App resets canvas to factory default.
            Serial.printf("EVT BTN 1 RESET\n");
            Serial.flush();
            break;

        // ── BTN2: Global — Disconnect / Reconnect ─────────────────────────────
        case INPUT_BTN2_PRESS:
            Serial.printf("EVT BTN 2 DISCONNECT\n");
            Serial.flush();
            break;

        case INPUT_BTN2_LONG:
            Serial.printf("EVT BTN 2 RECONNECT\n");
            Serial.flush();
            break;

        // ── BTN3: Layer-specific (← / Pomodoro reset / Spotify prev) ──────────
        case INPUT_BTN3_PRESS:
            Serial.printf("EVT BTN 3 S\n");
            Serial.flush();
            break;

        case INPUT_BTN3_LONG:
            Serial.printf("EVT BTN 3 L\n");
            Serial.flush();
            break;

        // ── BTN4: Layer-specific (▶ / Pomodoro start-pause / Spotify play) ────
        case INPUT_BTN4_PRESS:
            Serial.printf("EVT BTN 4 S\n");
            Serial.flush();
            break;

        case INPUT_BTN4_LONG:
            Serial.printf("EVT BTN 4 L\n");
            Serial.flush();
            break;

        // ── BTN5: Layer-specific (→ / Pomodoro next session / Spotify next) ────
        case INPUT_BTN5_PRESS:
            Serial.printf("EVT BTN 5 S\n");
            Serial.flush();
            break;

        case INPUT_BTN5_LONG:
            Serial.printf("EVT BTN 5 L\n");
            Serial.flush();
            break;

        default:
            break;
    }
}