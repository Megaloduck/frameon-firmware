#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// hid_controller.h — USB HID input reports for the Frameon physical controller
//
// The ESP32-S3 presents as a USB composite device:
//   Interface 0/1: CDC ACM  — FRM packet send/receive (unchanged)
//   Interface 2:   HID      — controller input reports (this file)
//
// Windows uses the built-in HID class driver (hid.sys) for Interface 2 so
// no custom driver or INF is needed.  The device enumerates as:
//   VID 0x303A  PID 0x4001  "Frameon Controller"
//
// HID Report (Report ID = 0x01, payload = 7 bytes, total on wire = 8 bytes)
// ────────────────────────────────────────────────────────────────────────────
//  Offset  Field          Type    Range        Description
//   0      report_id      uint8   always 1     report identifier
//   1      enc_delta      int8    −127…+127    encoder steps since last report
//                                              + = CW (preset +)  − = CCW (−)
//   2-3    joy_x          uint16  0…4095       raw ADC, calibrated by app
//   4-5    joy_y          uint16  0…4095       raw ADC
//   6      buttons        uint8   bitmask      button currently held (see masks)
//   7      events         uint8   bitmask      one-shot events cleared each report
//
// buttons bitmask (field byte 6)
//   bit 0  ENC_PRESS   encoder button held down
//   bit 1  JOY_PRESS   joystick SW held down
//   bit 2  BTN1        push button 1
//   bit 3  BTN2        push button 2
//   bit 4  BTN3        push button 3
//   bit 5  BTN4        push button 4
//   bit 6  BTN5        push button 5
//   bit 7  reserved
//
// events bitmask (field byte 7) — set for one report cycle then cleared
//   bit 0  ENC_LONG    encoder long-hold fired
//   bit 1  JOY_LONG    joystick SW long-hold fired
//   bit 2  BTN1_LONG   BTN1 long-hold
//   bit 3  BTN2_LONG   BTN2 long-hold
//   bit 4  BTN3_LONG   BTN3 long-hold
//   bit 5  BTN4_LONG   BTN4 long-hold
//   bit 6  BTN5_LONG   BTN5 long-hold
//   bit 7  reserved
// ─────────────────────────────────────────────────────────────────────────────

#include <stdint.h>
#include <stdbool.h>

// ── USB identifiers ───────────────────────────────────────────────────────────
#define FRAMEON_USB_VID      0x303A  // Espressif Systems
#define FRAMEON_USB_PID      0x4001  // Frameon Controller
#define FRAMEON_HID_REPORT_ID  0x01
#define FRAMEON_HID_REPORT_SIZE  7   // payload bytes (excluding report ID)

// ── Button bitmasks ───────────────────────────────────────────────────────────
#define HID_BTN_ENC_PRESS  (1 << 0)
#define HID_BTN_JOY_PRESS  (1 << 1)
#define HID_BTN_1          (1 << 2)
#define HID_BTN_2          (1 << 3)
#define HID_BTN_3          (1 << 4)
#define HID_BTN_4          (1 << 5)
#define HID_BTN_5          (1 << 6)

// ── Event bitmasks (one-shot) ─────────────────────────────────────────────────
#define HID_EVT_ENC_LONG   (1 << 0)
#define HID_EVT_JOY_LONG   (1 << 1)
#define HID_EVT_BTN1_LONG  (1 << 2)
#define HID_EVT_BTN2_LONG  (1 << 3)
#define HID_EVT_BTN3_LONG  (1 << 4)
#define HID_EVT_BTN4_LONG  (1 << 5)
#define HID_EVT_BTN5_LONG  (1 << 6)

// ── Report struct (matches on-wire layout) ────────────────────────────────────
#pragma pack(push, 1)
typedef struct {
    int8_t   encDelta;   // encoder steps since last report
    uint16_t joyX;       // raw ADC 0–4095
    uint16_t joyY;       // raw ADC 0–4095
    uint8_t  buttons;    // currently-held buttons bitmask
    uint8_t  events;     // one-shot events bitmask
} FrameonHidReport;
#pragma pack(pop)

static_assert(sizeof(FrameonHidReport) == FRAMEON_HID_REPORT_SIZE,
              "HID report size mismatch");

// ── HID descriptor ─────────────────────────────────────────────────────────────
// Vendor-defined usage page (0xFF00).  Windows HID class driver loads without
// any INF or driver install.  Report ID 1 wraps the 7-byte FrameonHidReport.
extern const uint8_t  kFrameonHidDescriptor[];
extern const uint16_t kFrameonHidDescriptorLen;

// ── Public API ────────────────────────────────────────────────────────────────

/// Call once from setup(), BEFORE Serial.begin(), so TinyUSB registers
/// both the CDC and HID interfaces before the USB stack starts.
void hidControllerBegin();

/// Call from loop() or the input task after computing the new report state.
/// Sends the HID report to the host if any field changed since the last send.
/// Returns true if a report was sent, false if nothing changed or USB not ready.
bool hidControllerSendIfChanged(const FrameonHidReport& report);

/// Force-send a report regardless of whether it changed (useful on connect).
bool hidControllerSendReport(const FrameonHidReport& report);