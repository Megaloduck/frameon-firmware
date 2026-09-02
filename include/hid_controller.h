// include/hid_controller.h
//
// ─────────────────────────────────────────────────────────────────────────────
// Frameon USB HID controller — composite CDC + HID on ESP32-S3 native USB.
//
// ── FIX: HID report struct updated to match Dart hid_report.dart ─────────────
//
//  OLD struct (caused "app can't send display" bug):
//    int8_t   encDelta;
//    uint16_t joyX;
//    uint16_t joyY;       ← raw ADC, 2 bytes
//    uint8_t  buttons;
//    uint8_t  events;     ← no taps field
//
//  NEW struct (matches Dart hid_report.dart layout):
//    int8_t   encDelta;
//    uint16_t joyX;
//    uint8_t  brightness; ← firmware-applied brightness (1 byte, was joyY uint16)
//    uint8_t  buttons;
//    uint8_t  events;
//    uint8_t  taps;       ← short-press one-shot bitmask (new field)
//
//  Why the old struct broke sendToDevice():
//    Dart's hid_report.dart reads byte[6] as `events` and bit 0 as `encLong`.
//    With the old struct, byte[6] is the firmware's `buttons` (held-button
//    bitmask, not one-shot). Bit 0 of buttons = HID_BTN_ENC_PRESS.
//    Result: `encLong` fired on every HID report while the encoder button was
//    physically held — toggling deviceLocked true/false at 20 ms intervals.
//    Whenever deviceLocked was true during a send attempt, sendToDevice()
//    returned early with "Display is locked." even though it wasn't.
//
//    Additionally, short-press (tap) events were never put in any field —
//    `pb()` discarded r==1 (short release), so btn1Tap/btn2Tap/… were always
//    false, and BTN1 "Sync display" never fired.
//
//  Fix:
//    1. Replace uint16_t joyY with uint8_t brightness + uint8_t taps (total
//       payload stays 7 bytes — no size change, no Windows driver reinstall).
//    2. Add HID_TAP_* bitmask defines matching kHidTap* in Dart.
//    3. Update the HID descriptor in hid_controller.cpp to describe the new layout.
//    4. Update inputhelper.cpp to:
//         • Fill report.brightness from displayBrightness (adjusted by joyY).
//         • Fill report.taps for short-press releases (r==1 in Button::poll).
//         • Remove report.joyY.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once

#include <stdint.h>

// ── USB identity ───────────────────────────────────────────────────────────────
#define FRAMEON_USB_VID  0x303A
#define FRAMEON_USB_PID  0x4001

// ── HID report constants ───────────────────────────────────────────────────────
#define FRAMEON_HID_REPORT_ID    1
#define FRAMEON_HID_REPORT_SIZE  7   // bytes of payload (excl. report ID)

// ── Button masks (buttons field — held state) ──────────────────────────────────
#define HID_BTN_ENC_PRESS  (1 << 0)
#define HID_BTN_JOY_PRESS  (1 << 1)
#define HID_BTN_1          (1 << 2)
#define HID_BTN_2          (1 << 3)
#define HID_BTN_3          (1 << 4)
#define HID_BTN_4          (1 << 5)
#define HID_BTN_5          (1 << 6)

// ── Event masks — long-press one-shot (events field) ──────────────────────────
#define HID_EVT_ENC_LONG   (1 << 0)
#define HID_EVT_JOY_LONG   (1 << 1)
#define HID_EVT_BTN1_LONG  (1 << 2)
#define HID_EVT_BTN2_LONG  (1 << 3)
#define HID_EVT_BTN3_LONG  (1 << 4)
#define HID_EVT_BTN4_LONG  (1 << 5)
#define HID_EVT_BTN5_LONG  (1 << 6)

// ── Tap masks — short-press one-shot (taps field) ─────────────────────────────
// FIX: new field. Bit positions match kHidTap* in Dart's hid_report.dart.
#define HID_TAP_ENC   (1 << 0)
#define HID_TAP_JOY   (1 << 1)
#define HID_TAP_BTN1  (1 << 2)
#define HID_TAP_BTN2  (1 << 3)
#define HID_TAP_BTN3  (1 << 4)
#define HID_TAP_BTN4  (1 << 5)
#define HID_TAP_BTN5  (1 << 6)

// ── Report struct ──────────────────────────────────────────────────────────────
// FIX: uint16_t joyY removed; uint8_t brightness + uint8_t taps added.
// sizeof == 7 bytes (unchanged) so FRAMEON_HID_REPORT_SIZE is still correct
// and Windows does not require a driver reinstall (same report size).
// The HID descriptor in hid_controller.cpp is updated to match.
#pragma pack(push, 1)
typedef struct {
    int8_t   encDelta;    // encoder steps since last report (+CW, -CCW)
    uint16_t joyX;        // raw ADC 0-4095 (app applies dead-zone + calibration)
    uint8_t  brightness;  // FIX: was joyY uint16. Now: firmware-applied display
                          //      brightness 0-255, updated by joystick Y in inputTask.
    uint8_t  buttons;     // held-button bitmask (HID_BTN_*)
    uint8_t  events;      // long-press one-shot bitmask (HID_EVT_*)
    uint8_t  taps;        // FIX: new field. Short-press one-shot (HID_TAP_*).
                          //      Was discarded by old pb() lambda (r==1 case ignored).
} FrameonHidReport;
#pragma pack(pop)

static_assert(sizeof(FrameonHidReport) == FRAMEON_HID_REPORT_SIZE,
              "HID report size mismatch — check struct vs FRAMEON_HID_REPORT_SIZE");

// ── HID descriptor ─────────────────────────────────────────────────────────────
extern const uint8_t  kFrameonHidDescriptor[];
extern const uint16_t kFrameonHidDescriptorLen;

// ── Public API ─────────────────────────────────────────────────────────────────

/// Call once from setup(), BEFORE Serial.begin().
void hidControllerBegin();

/// Send report if changed. Returns true if a report was sent.
bool hidControllerSendIfChanged(const FrameonHidReport& report);

/// Force-send regardless of change (e.g. on connect).
bool hidControllerSendReport(const FrameonHidReport& report);