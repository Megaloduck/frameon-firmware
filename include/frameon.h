#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// frameon.h — GPIO assignments + firmware constants
//
// ESP32-S3 · HUB75E 64×32 P4-2121 panel
//
// Controller layout (revised — single encoder + joystick + 5 buttons):
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │  Block                  │  Pins used                                                                                                       │
//   │─────────────│───────────────────────────────────────────────────────│
//   │  HUB75E display │  1,2,4,5,12,13,14,15,16,38,39,40,41,42                                               (14 pins)│
//   │  Rotary encoder  │  17,18,21                                                                                            (3 pins) │
//   │  Joystick              │  6,7,8                                                                                                  (3 pins) │
//   │  Push buttons     │  9,10,11,47,48                                                                                     (5 pins) │
//   └─────────────────────────────────────────────────────────────────────┘
//
// GPIO3,45,46  strapping pins — left unassigned.
// GPIO19,20    native USB D−/D+ (ESP32-S3 OTG) — RESERVED for future use.
//              Disabled via -DARDUINO_USB_MODE=0 / -DARDUINO_USB_CDC_ON_BOOT=0.
// GPIO33,34    OPI PSRAM data lines on S3-WROOM-2 — left unassigned.
// ADC2 (GPIO11–GPIO20) avoided for Wi-Fi-concurrent ADC; joystick uses
// ADC1 (GPIO1–GPIO10) only.  GPIO9–GPIO11 are digital-only (no ADC).
// ─────────────────────────────────────────────────────────────────────────────

// ─── HUB75E display (MatrixPanel_I2S_DMA) ────────────────────────────────────
#define PIN_R1      4
#define PIN_G1      5
#define PIN_B1     12
#define PIN_R2     13
#define PIN_G2     14
#define PIN_B2     15
#define PIN_A      38
#define PIN_B      39
#define PIN_C      40
#define PIN_D      41
#define PIN_E      42
#define PIN_CLK     2
#define PIN_LAT     1
#define PIN_OE     16

// ─── KY-040 Rotary Encoder (single) ──────────────────────────────────────────
//   Rotation:  ISR on CLK rising edge; DT level determines CW vs CCW.
//   Button:    Active LOW, internal PULLUP.  Short / long press (≥ 600 ms).
//
//   Actions (from controller spec):
//     CW          → Preset switch +      (app-side navigation)
//     CCW         → Preset switch −      (app-side navigation)
//     Short press → Check preset number  (app-side query)
//     Long hold   → Lock / Unlock display (firmware toggle + EVT to app)
#define PIN_ENC_CLK  17   // CLK → GPIO17  (digital in, interrupt-capable)
#define PIN_ENC_DT   18   // DT  → GPIO18  (digital in)
#define PIN_ENC_SW   21   // SW  → GPIO21  (digital in, active LOW, PULLUP)

// ─── KY-023 Analog Joystick ──────────────────────────────────────────────────
//   Axes:    12-bit ADC1, full 0–3.3 V range (ADC_11db attenuation).
//   Button:  Active LOW, internal PULLUP.  Short / long press (≥ 600 ms).
//
//   Actions (from controller spec, same across all layer types):
//     Up          → Brightness +    (firmware applies + EVT BRIGHT)
//     Down        → Brightness −    (firmware applies + EVT BRIGHT)
//     Right       → Opacity +       (app-side, EVT JOY OPACITY +)
//     Left        → Opacity −       (app-side, EVT JOY OPACITY −)
//     Short press → Layer-specific tap   (EVT JOY PRESS)
//     Long hold   → Edit / Save layer   (EVT JOY HOLD)
#define PIN_JOY_VRX  6   // X-axis ADC → GPIO6  (ADC1 channel 5)
#define PIN_JOY_VRY  7   // Y-axis ADC → GPIO7  (ADC1 channel 6)
#define PIN_JOY_SW   8   // SW button  → GPIO8  (digital in, active LOW, PULLUP)

// ─── Push Buttons (5×) ───────────────────────────────────────────────────────
//   All: Active LOW, internal PULLUP.  Short / long press (≥ 600 ms).
//
//   BTN1 — Global navigation  (top row, left)
//     Short → Sync display
//     Long  → Reset to default
//
//   BTN2 — Global navigation  (top row, right)
//     Short → Disconnect
//     Long  → Reconnect
//
//   BTN3 — Layer-specific     (bottom row, left  ← prev / reset)
//     Pomodoro → Short: Reset timer
//     Spotify  → Short: Previous song  |  Long: Volume −
//
//   BTN4 — Layer-specific     (bottom row, centre ▶ play/pause)
//     Pomodoro → Short: Start / Pause
//     Spotify  → Short: Play / Pause
//
//   BTN5 — Layer-specific     (bottom row, right  → next / session)
//     Pomodoro → Short: Next session
//     Spotify  → Short: Next song  |  Long: Volume +
#define PIN_BTN1  9    // GPIO9
#define PIN_BTN2  10   // GPIO10
#define PIN_BTN3  11   // GPIO11
#define PIN_BTN4  47   // GPIO47
#define PIN_BTN5  48   // GPIO48

// ─── USB native pins — RESERVED, do not assign ───────────────────────────────
//   GPIO19  USB D−  (ESP32-S3 internal OTG peripheral)
//   GPIO20  USB D+  (ESP32-S3 internal OTG peripheral)

// ─── Panel geometry ──────────────────────────────────────────────────────────
#define PANEL_CHAIN         1
#define DEFAULT_BRIGHTNESS  128
#define PANEL_WIDTH        64
#define PANEL_HEIGHT       64
#define REAL_HEIGHT        32

// ─── Packet framing ──────────────────────────────────────────────────────────
#define FRM_MAGIC_0    0x46   // 'F'
#define FRM_MAGIC_1    0x52   // 'R'
#define FRM_MAGIC_2    0x4D   // 'M'
#define FRM_VERSION    0x03
#define FRM_NEXT       0x4E   // 'N' — queue as next-song preload

#define HEADER_SIZE    80
#define CRC_SIZE        2

// ─── Clock flag bits ─────────────────────────────────────────────────────────
#define CLK_FLAG_PRESENT    0x01
#define CLK_FLAG_H12        0x02
#define CLK_FLAG_SECONDS    0x04
#define CLK_FLAG_DATE       0x08
#define CLK_FLAG_BLINK      0x10
#define CLK_FLAG_AMPM       0x20

// ─── Clock layout styles ─────────────────────────────────────────────────────
#define CLK_LAYOUT_CLASSIC         0
#define CLK_LAYOUT_ANALOG          1
#define CLK_LAYOUT_WEEKDAY_PREFIX  2
#define CLK_LAYOUT_STACKED         3
#define CLK_LAYOUT_SECONDS_BAR     4
#define CLK_LAYOUT_DUAL_TIMEZONE   5

// ─── Analog flag bits ────────────────────────────────────────────────────────
#define ANALOG_FACE_MASK         0x03
#define ANALOG_FACE_CARDINAL     0x00
#define ANALOG_FACE_ALL_DOTS     0x01
#define ANALOG_FACE_TICKS        0x02
#define ANALOG_FACE_NONE         0x03
#define ANALOG_SHOW_SECOND_HAND  0x04
#define ANALOG_SHOW_DIGITAL      0x08

// ─── Pomodoro flag bits ──────────────────────────────────────────────────────
#define POMO_FLAG_PRESENT    0x01
#define POMO_FLAG_RUNNING    0x02
#define POMO_FLAG_SECONDS    0x04
#define POMO_FLAG_SESSION    0x08
#define POMO_FLAG_BLINK      0x10

// ─── Serial responses ────────────────────────────────────────────────────────
#define RESP_ACK       0x06
#define RESP_NAK       0x15
#define RESP_ERR       0x1B

// ─── Buffer limits ───────────────────────────────────────────────────────────
#define MAX_FRAMES     300
#define FRAME_PIXELS   (PANEL_WIDTH * REAL_HEIGHT)
#define FRAME_BYTES    (FRAME_PIXELS * 2)
#define MAX_PAYLOAD    ((uint32_t)MAX_FRAMES * FRAME_BYTES)
#define MAX_PACKET     (HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE)