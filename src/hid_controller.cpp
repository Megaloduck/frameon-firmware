// src/hid_controller.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// Frameon USB HID controller — composite CDC + HID on ESP32-S3 native USB.
//
// ── FIX: HID descriptor updated to match the new FrameonHidReport struct ──────
//
//  Old descriptor described:
//    enc_delta  int8   (1 byte)
//    joy_x      uint16 (2 bytes)
//    joy_y      uint16 (2 bytes)  ← removed
//    buttons    uint8  (1 byte)
//    events     uint8  (1 byte)
//    Total payload = 7 bytes
//
//  New descriptor describes:
//    enc_delta  int8   (1 byte)
//    joy_x      uint16 (2 bytes)
//    brightness uint8  (1 byte)  ← was joy_y uint16
//    buttons    uint8  (1 byte)
//    events     uint8  (1 byte)
//    taps       uint8  (1 byte)  ← new
//    Total payload = 7 bytes  (unchanged — Windows does not re-enumerate driver)
//
//  IMPORTANT: after reflashing with the new descriptor, Windows may have
//  cached the old one. If behaviour is still wrong after reflash, unplug the
//  device, open Device Manager, delete the "Frameon Controller" HID device,
//  then replug. Windows will re-read the descriptor from the device.
// ─────────────────────────────────────────────────────────────────────────────

#include "hid_controller.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHID.h>
#include <string.h>

// ── HID descriptor ─────────────────────────────────────────────────────────────
// Vendor usage page (0xFF00) — avoids conflicting with gamepad / mouse usages.
// Report ID 1 → 7 bytes:
//   enc_delta (int8) + joy_x (uint16) + brightness (uint8)
//   + buttons (uint8) + events (uint8) + taps (uint8)
const uint8_t kFrameonHidDescriptor[] = {
    0x06, 0x00, 0xFF,  // Usage Page (Vendor 0xFF00)
    0x09, 0x01,        // Usage (Frameon Controller)
    0xA1, 0x01,        // Collection (Application)

    0x85, 0x01,        //   Report ID (1)

    // ── enc_delta: signed int8, −127…+127, relative ──────────────────────────
    0x09, 0x02,        //   Usage (Encoder)
    0x15, 0x81,        //   Logical Minimum (−127)
    0x25, 0x7F,        //   Logical Maximum (+127)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x06,        //   Input (Data, Variable, Relative)

    // ── joy_x: uint16, 0…4095, absolute ──────────────────────────────────────
    0x09, 0x03,        //   Usage (Joystick X)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x0F,  //   Logical Maximum (4095)
    0x75, 0x10,        //   Report Size (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // ── brightness: uint8, 0…255, absolute ───────────────────────────────────
    // FIX: was joy_y (uint16, 2 bytes). Now brightness (uint8, 1 byte).
    // Joystick Y no longer appears in the HID report — the firmware applies
    // its value directly to the display brightness and reports the result here.
    0x09, 0x04,        //   Usage (Brightness)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0xFF,        //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)  ← was 0x10 (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // ── buttons: uint8, held-button bitmask ───────────────────────────────────
    0x09, 0x05,        //   Usage (Buttons)
    0x15, 0x00,
    0x25, 0xFF,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x02,

    // ── events: uint8, long-press one-shot bitmask ────────────────────────────
    0x09, 0x06,        //   Usage (Events)
    0x15, 0x00,
    0x25, 0xFF,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x02,

    // ── taps: uint8, short-press one-shot bitmask ─────────────────────────────
    // FIX: new field. Short-press releases that were previously discarded by
    // the pb() lambda are now captured here.
    0x09, 0x07,        //   Usage (Taps)
    0x15, 0x00,
    0x25, 0xFF,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x02,

    0xC0               // End Collection
};
const uint16_t kFrameonHidDescriptorLen = sizeof(kFrameonHidDescriptor);

// ── USBHID device subclass ─────────────────────────────────────────────────────

class _FrameonHIDDevice : public USBHIDDevice {
public:
    _FrameonHIDDevice() = default;

    void begin(USBHID& hid) {
        hid.addDevice(this, kFrameonHidDescriptorLen);
        _pHid = &hid;
    }

    uint16_t _onGetDescriptor(uint8_t* buffer) override {
        memcpy(buffer, kFrameonHidDescriptor, kFrameonHidDescriptorLen);
        return kFrameonHidDescriptorLen;
    }

    bool send(const FrameonHidReport& report) {
        if (!_pHid) return false;
        return _pHid->SendReport(FRAMEON_HID_REPORT_ID, &report, sizeof(report));
    }

private:
    USBHID* _pHid = nullptr;
};

// ── Module-private state ───────────────────────────────────────────────────────

static USBHID            _hid;
static _FrameonHIDDevice _device;
static FrameonHidReport  _lastReport{};
static bool              _started = false;

// ── Public API ─────────────────────────────────────────────────────────────────

void hidControllerBegin() {
    if (_started) return;

    USB.manufacturerName("Frameon");
    USB.productName("Frameon Controller");
    USB.serialNumber("FRM-001");
    USB.VID(FRAMEON_USB_VID);
    USB.PID(FRAMEON_USB_PID);

    _device.begin(_hid);
    _hid.begin();

    memset(&_lastReport, 0, sizeof(_lastReport));
    _started = true;
}

bool hidControllerSendIfChanged(const FrameonHidReport& report) {
    // Always send if one-shot fields are pending (they must not be dropped).
    const bool hasOneShots = (report.events != 0 || report.taps != 0);
    const bool changed     = (memcmp(&report, &_lastReport, sizeof(report)) != 0);
    if (!hasOneShots && !changed) return false;

    const bool sent = _device.send(report);
    if (sent) _lastReport = report;
    return sent;
}

bool hidControllerSendReport(const FrameonHidReport& report) {
    const bool sent = _device.send(report);
    if (sent) _lastReport = report;
    return sent;
}