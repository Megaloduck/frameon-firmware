#pragma once

#include <Arduino.h>
#include <NimBLEDevice.h>
#include "config.h"

// ═══════════════════════════════════════════════════════════════════════════
//  ble_server.h
//
//  Implements the Frameon BLE GATT service.  Every UUID and command byte
//  mirrors lib/core/ble/ble_uuids.dart exactly so the Flutter app works
//  without modification.
//
//  Wire protocol (from ble_service.dart):
//
//    SEND STILL FRAME:
//      1. CONTROL   ← [CMD_FRAME_BEGIN, frameIdx=0, total=1]
//      2. FRAME_DATA← chunks of RGB565 bytes (negotiated MTU-3 per write)
//      3. CONTROL   ← [CMD_FRAME_COMMIT]
//      4. STATUS    → [STATUS_OK]   (notify back to phone)
//
//    SEND GIF:
//      0. GIF_META  ← [frameCount u8, dur0 u16-BE, dur1 u16-BE, …]
//      Then for each frame i:
//        repeat steps 1-4 above with frameIdx = i, total = frameCount
//
//    SET CLOCK:
//      CLOCK_CONFIG ← [epoch u32-BE (4 bytes), flags u8]
//        flags: bit0=is24h  bit1=showSeconds  bit2=showDate
// ═══════════════════════════════════════════════════════════════════════════

enum class TransferState { IDLE, RECEIVING, COMMITTING };

class BleServer {
public:
    BleServer();

    void begin();                       // call once in setup()
    void notifyStatus(uint8_t byte);    // send STATUS_* back to phone
    bool isConnected() const { return _connected; }

private:
    // ── NimBLE handles ────────────────────────────────────────────────────────
    NimBLEServer*         _server        = nullptr;
    NimBLEService*        _service       = nullptr;
    NimBLECharacteristic* _charFrameData = nullptr;
    NimBLECharacteristic* _charControl   = nullptr;
    NimBLECharacteristic* _charStatus    = nullptr;
    NimBLECharacteristic* _charClockCfg  = nullptr;
    NimBLECharacteristic* _charGifMeta   = nullptr;

    // ── Transfer state ────────────────────────────────────────────────────────
    TransferState _txState       = TransferState::IDLE;
    uint8_t*      _frameBuf      = nullptr;  // PSRAM allocated, FRAME_BUF_SIZE
    size_t        _frameBufPos   = 0;
    uint8_t       _txFrameIdx    = 0;
    uint8_t       _txTotalFrames = 1;

    // ── GIF meta (received before frame data) ─────────────────────────────────
    uint16_t _gifDurations[GIF_MAX_FRAMES] = {};
    uint8_t  _gifFrameCount = 0;

    bool    _connected   = false;
    uint8_t _currentMode = MODE_STILL;

    // ── GATT callbacks ────────────────────────────────────────────────────────
    struct SrvCb : public NimBLEServerCallbacks {
        BleServer* o;
        explicit SrvCb(BleServer* owner) : o(owner) {}
        void onConnect(NimBLEServer*)    override;
        void onDisconnect(NimBLEServer*) override;
    };
    struct FrameCb : public NimBLECharacteristicCallbacks {
        BleServer* o;
        explicit FrameCb(BleServer* owner) : o(owner) {}
        void onWrite(NimBLECharacteristic*) override;
    };
    struct CtrlCb : public NimBLECharacteristicCallbacks {
        BleServer* o;
        explicit CtrlCb(BleServer* owner) : o(owner) {}
        void onWrite(NimBLECharacteristic*) override;
    };
    struct ClockCb : public NimBLECharacteristicCallbacks {
        BleServer* o;
        explicit ClockCb(BleServer* owner) : o(owner) {}
        void onWrite(NimBLECharacteristic*) override;
    };
    struct GifMetaCb : public NimBLECharacteristicCallbacks {
        BleServer* o;
        explicit GifMetaCb(BleServer* owner) : o(owner) {}
        void onWrite(NimBLECharacteristic*) override;
    };

    // ── Handlers ─────────────────────────────────────────────────────────────
    void _handleFrameData (const uint8_t* data, size_t len);
    void _handleControl   (const uint8_t* data, size_t len);
    void _handleClockCfg  (const uint8_t* data, size_t len);
    void _handleGifMeta   (const uint8_t* data, size_t len);
    void _commitFrame();
    void _reset();
};

BleServer& Ble();