#pragma once

// ─── GPIO pin assignments ─────────────────────────────────────────────────────
//
// ESP32-S3 · HUB75E 64×32 P4-2121 panel + controller inputs
//
// ┌─────────────────────────────────────────────────────────────────────┐
// │  Block          │  Pins used                                        │
// │─────────────────│───────────────────────────────────────────────────│
// │  HUB75E display │  1,2,4,5,12,13,14,15,16,38,39,40,41,42  (14 pins)│
// │  Encoder 1      │  17,18,21                                (3 pins) │
// │  Encoder 2      │  35,36,37                                (3 pins) │
// │  Joystick       │  6,7,8                                   (3 pins) │
// │  Push buttons   │  9,10,11,47,48                           (5 pins) │
// └─────────────────────────────────────────────────────────────────────┘
//
// GPIO3,45,46 are strapping pins — left unassigned.
// GPIO19,20   are native USB D−/D+ (ESP32-S3 OTG) — left RESERVED.
//             Disabled via -DARDUINO_USB_MODE=0 / -DARDUINO_USB_CDC_ON_BOOT=0
//             but kept free to preserve the option of native USB CDC later.
// GPIO33,34   are OPI PSRAM data lines on S3-WROOM-2 — left unassigned.
// ADC2 (GPIO11–GPIO20) is avoided for Wi-Fi-concurrent ADC; joystick
// uses ADC1 (GPIO1–GPIO10) only. GPIO9–GPIO11 are digital-only (no ADC).

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

// ─── KY-040 Rotary Encoder 1 ─────────────────────────────────────────────────
//   Rotation:  ISR on CLK rising edge; DT level determines direction.
//   Button:    Active LOW, internal PULLUP. Short press / long press (≥600 ms).
//   Function:  Brightness control (CW +8 / CCW -8 / long press = reset).
#define PIN_ENC1_CLK  17   // CLK → GPIO17  (digital in, interrupt-capable)
#define PIN_ENC1_DT   18   // DT  → GPIO18  (digital in)
#define PIN_ENC1_SW   21   // SW  → GPIO21  (digital in, active LOW, PULLUP)

// ─── KY-040 Rotary Encoder 2 ─────────────────────────────────────────────────
//   Rotation:  ISR on CLK rising edge; DT level determines direction.
//   Button:    Active LOW, internal PULLUP. Short press / long press (≥600 ms).
//   Function:  Reserved — extend inputApplyEvent() as needed.
#define PIN_ENC2_CLK  3         // CLK → GPIO35  (digital in, interrupt-capable)
#define PIN_ENC2_DT   46       // DT  → GPIO36  (digital in)
#define PIN_ENC2_SW   45     // SW  → GPIO37  (digital in, active LOW, PULLUP)

// ─── KY-023 Analog Joystick ──────────────────────────────────────────────────
//   Axes:    12-bit ADC1, full 0–3.3 V range (ADC_11db attenuation).
//   Button:  Active LOW, internal PULLUP. Short press / long press (≥600 ms).
//   Function: Directional navigation events (UP/DOWN/LEFT/RIGHT/CENTER).
#define PIN_JOY_VRX    6   // VRx → GPIO6   (ADC1_CH5 — 0–3.3 V)
#define PIN_JOY_VRY    7   // VRy → GPIO7   (ADC1_CH6 — 0–3.3 V)
#define PIN_JOY_SW     8   // SW  → GPIO8   (digital in, active LOW, PULLUP)

// ─── SMD Push Buttons (×5) ───────────────────────────────────────────────────
//   All active LOW with internal PULLUP. Debounced at 40 ms in inputTask.
//   Short press (< 600 ms) and long press (≥ 600 ms) events available.
//   Default functions are reserved — map them in inputApplyEvent() as needed.
//
//   Physical layout (as viewed on device):
//     [ BTN1 ] [ BTN2 ] [ BTN3 ]
//              [ BTN4 ] [ BTN5 ]
//
#define PIN_BTN1       9   // → GPIO9   (digital in, active LOW, PULLUP)
#define PIN_BTN2      10   // → GPIO10  (digital in, active LOW, PULLUP)
#define PIN_BTN3      11   // → GPIO11  (digital in, active LOW, PULLUP)
#define PIN_BTN4      47   // → GPIO47  (digital in, active LOW, PULLUP)
#define PIN_BTN5      48   // → GPIO48  (digital in, active LOW, PULLUP)
//
// Note: GPIO47 was formerly PIN_TOUCH (TTP223B) — now repurposed as BTN4.
//       GPIO47 / GPIO48 are adjacent castellated pads on the WROOM-2, which
//       keeps the button cluster routing tight on the PCB.

// ─── USB native pins — RESERVED, do not assign ───────────────────────────────
//   GPIO19  USB D−  (ESP32-S3 internal OTG peripheral)
//   GPIO20  USB D+  (ESP32-S3 internal OTG peripheral)
//   Kept free so native USB CDC can be enabled in future without a PCB respin.

// ─── External RESET circuit ──────────────────────────────────────────────────
//   The RESET button connects to the dedicated EN pin (not a GPIO).
//   Typical circuit: EN → 10kΩ pull-up to 3.3V, 100nF to GND, RESET switch to GND.

/ ─── Panel geometry ──────────────────────────────────────────────────────────
#define PANEL_CHAIN         1
#define DEFAULT_BRIGHTNESS  128
#define PANEL_WIDTH        64
#define PANEL_HEIGHT       64
#define REAL_HEIGHT        32
 
// ─── Packet framing ──────────────────────────────────────────────────────────
#define FRM_MAGIC_0    0x46   // 'F'
#define FRM_MAGIC_1    0x52   // 'R'
#define FRM_MAGIC_2    0x4D   // 'M'
#define FRM_VERSION    0x03   // v2.0 — 80-byte header with clock v1.6 extension
#define FRM_NEXT       0x4E   // 'N' — queue as next-song preload
 
#define HEADER_SIZE    80     // v2.0: bumped from 68 for clock v1.6 extension
#define CRC_SIZE        2
 
// ─── Clock flag bits (clockFlags byte, offset 29) ────────────────────────────
#define CLK_FLAG_PRESENT    0x01  // clock layer is active
#define CLK_FLAG_H12        0x02  // 12-hour format (else 24-hour)
#define CLK_FLAG_SECONDS    0x04  // show seconds
#define CLK_FLAG_DATE       0x08  // show date below time
#define CLK_FLAG_BLINK      0x10  // blink colon every 500 ms
#define CLK_FLAG_AMPM       0x20  // show AM/PM (only meaningful with CLK_FLAG_H12)
 
// ─── Clock layout styles (v1.6, byte [68]) ──────────────────────────────────
// Must match Dart ClockLayoutStyle enum order.
#define CLK_LAYOUT_CLASSIC         0
#define CLK_LAYOUT_ANALOG          1
#define CLK_LAYOUT_WEEKDAY_PREFIX  2
#define CLK_LAYOUT_STACKED         3
#define CLK_LAYOUT_SECONDS_BAR     4
#define CLK_LAYOUT_DUAL_TIMEZONE   5
 
// ─── Analog flag bits (analogFlags byte, offset 69) ─────────────────────────
#define ANALOG_FACE_MASK         0x03  // bits 0-1: AnalogFaceStyle
#define ANALOG_FACE_CARDINAL     0x00  //   4 dots at 12/3/6/9
#define ANALOG_FACE_ALL_DOTS     0x01  //   12 dots, one per hour
#define ANALOG_FACE_TICKS        0x02  //   4 tick lines at 12/3/6/9
#define ANALOG_FACE_NONE         0x03  //   bare rim
#define ANALOG_SHOW_SECOND_HAND  0x04  // bit 2
#define ANALOG_SHOW_DIGITAL      0x08  // bit 3
 
// ─── Pomodoro flag bits (pomodoroFlags byte, offset 52) ──────────────────────
#define POMO_FLAG_PRESENT    0x01  // pomodoro layer is active
#define POMO_FLAG_RUNNING    0x02  // timer was running at commit
#define POMO_FLAG_SECONDS    0x04  // show seconds
#define POMO_FLAG_SESSION    0x08  // show session dots
#define POMO_FLAG_BLINK      0x10  // blink colon every 500 ms
 
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
 