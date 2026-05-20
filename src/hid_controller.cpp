// src/hid_controller.cpp
//
// ─────────────────────────────────────────────────────────────────────────────
// Frameon USB HID controller — composite CDC + HID on ESP32-S3 native USB.
//
// Uses the Arduino ESP32 core 2.x USBHID API (built on TinyUSB).
// The CDC interface (Serial) carries FRM packets as before.
// The HID interface carries 8-byte input reports at USB interrupt speed.
//
// Call order in setup():
//   1. hidControllerBegin()   ← registers HID before USB stack starts
//   2. Serial.setRxBufferSize / Serial.begin()  ← starts CDC
//
// The USB stack starts automatically once both interfaces are registered.
// Windows loads hid.sys for the HID interface and usbser.sys for CDC —
// no custom driver or INF required.
// ─────────────────────────────────────────────────────────────────────────────

#include "hid_controller.h"
#include <Arduino.h>
#include <USB.h>
#include <USBHID.h>
#include <string.h>

// ── HID descriptor ─────────────────────────────────────────────────────────────
// Vendor usage page (0xFF00) — avoids conflicting with gamepad / mouse usages.
// Report ID 1 → 7 bytes: enc_delta(int8) + joy_x(uint16) + joy_y(uint16)
//                         + buttons(uint8) + events(uint8)
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

    // ── joy_y: uint16, 0…4095, absolute ──────────────────────────────────────
    0x09, 0x04,        //   Usage (Joystick Y)
    0x15, 0x00,
    0x26, 0xFF, 0x0F,
    0x75, 0x10,
    0x95, 0x01,
    0x81, 0x02,

    // ── buttons: uint8, bitmask of currently-held buttons ─────────────────────
    0x09, 0x05,        //   Usage (Buttons)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0xFF,        //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)

    // ── events: uint8, one-shot event bitmask ─────────────────────────────────
    0x09, 0x06,        //   Usage (Events)
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

    /// Register with the USBHID instance and start the HID stack.
    void begin(USBHID& hid) {
        hid.addDevice(this, kFrameonHidDescriptorLen);
        _pHid = &hid;
    }

    /// Provide our descriptor to TinyUSB when requested.
    uint16_t _onGetDescriptor(uint8_t* buffer) override {
        memcpy(buffer, kFrameonHidDescriptor, kFrameonHidDescriptorLen);
        return kFrameonHidDescriptorLen;
    }

    /// Send a HID input report (report ID + 7 payload bytes = 8 bytes total).
    bool send(const FrameonHidReport& report) {
        if (!_pHid) return false;
        return _pHid->SendReport(FRAMEON_HID_REPORT_ID, &report, sizeof(report));
    }

private:
    USBHID* _pHid = nullptr;
};

// ── Module-private state ───────────────────────────────────────────────────────

static USBHID           _hid;
static _FrameonHIDDevice _device;
static FrameonHidReport  _lastReport{};
static bool              _started = false;

// ── Public API ─────────────────────────────────────────────────────────────────

void hidControllerBegin() {
    if (_started) return;

    // Set USB device identity (must call before USB stack starts).
    USB.manufacturerName("Frameon");
    USB.productName("Frameon Controller");
    USB.serialNumber("FRM-001");
    USB.VID(FRAMEON_USB_VID);
    USB.PID(FRAMEON_USB_PID);

    // Register HID device and start the HID stack.
    // The CDC (Serial) stack is registered automatically by the framework;
    // calling _hid.begin() after addDevice() signals TinyUSB to include
    // both CDC and HID in the composite device descriptor.
    _device.begin(_hid);
    _hid.begin();

    memset(&_lastReport, 0, sizeof(_lastReport));
    _started = true;
}

bool hidControllerSendIfChanged(const FrameonHidReport& report) {
    // Always send if one-shot events are pending (they must reach the host).
    const bool hasEvents = (report.events != 0);
    const bool changed   = (memcmp(&report, &_lastReport, sizeof(report)) != 0);
    if (!hasEvents && !changed) return false;

    const bool sent = _device.send(report);
    if (sent) _lastReport = report;
    return sent;
}

bool hidControllerSendReport(const FrameonHidReport& report) {
    const bool sent = _device.send(report);
    if (sent) _lastReport = report;
    return sent;
}