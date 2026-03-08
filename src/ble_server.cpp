#include "ble_server.h"
#include "display_manager.h"
#include "clock_manager.h"
#include <time.h>

// ═══════════════════════════════════════════════════════════════════════════
//  ble_server.cpp
// ═══════════════════════════════════════════════════════════════════════════

// ── Singleton ─────────────────────────────────────────────────────────────────

static BleServer* _instance = nullptr;

BleServer& Ble() {
    if (!_instance) _instance = new BleServer();
    return *_instance;
}

// ── Constructor ───────────────────────────────────────────────────────────────

BleServer::BleServer() {
    // Allocate frame buffer in PSRAM if available, else SRAM
    if (psramFound()) {
        _frameBuf = (uint8_t*) ps_malloc(FRAME_BUF_SIZE);
        LOG("Frame buffer allocated in PSRAM (%u bytes)", FRAME_BUF_SIZE);
    } else {
        _frameBuf = (uint8_t*) malloc(FRAME_BUF_SIZE);
        LOG("PSRAM not found — frame buffer in SRAM (%u bytes)", FRAME_BUF_SIZE);
    }
    if (!_frameBuf) {
        LOG("ERROR: failed to allocate frame buffer!");
    }
}

// ── begin() ───────────────────────────────────────────────────────────────────

void BleServer::begin() {
    NimBLEDevice::init(BLE_DEVICE_NAME);

    // Increase MTU so the phone can send ~244-byte chunks.
    // This matches kDefaultChunkSize = 244 in ble_uuids.dart.
    NimBLEDevice::setMTU(247);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new SrvCb(this));

    // ── Create service ────────────────────────────────────────────────────────
    _service = _server->createService(BLE_SERVICE_UUID);

    // FRAME_DATA: Write-without-response (phone uses writeWithoutResponse for speed)
    _charFrameData = _service->createCharacteristic(
        BLE_CHAR_FRAME_DATA,
        NIMBLE_PROPERTY::WRITE_NR   // NR = no response
    );
    _charFrameData->setCallbacks(new FrameCb(this));

    // CONTROL: Write-with-response
    _charControl = _service->createCharacteristic(
        BLE_CHAR_CONTROL,
        NIMBLE_PROPERTY::WRITE
    );
    _charControl->setCallbacks(new CtrlCb(this));

    // STATUS: Notify (ESP32 → phone)
    _charStatus = _service->createCharacteristic(
        BLE_CHAR_STATUS,
        NIMBLE_PROPERTY::NOTIFY
    );

    // CLOCK_CONFIG: Write-with-response
    _charClockCfg = _service->createCharacteristic(
        BLE_CHAR_CLOCK_CONFIG,
        NIMBLE_PROPERTY::WRITE
    );
    _charClockCfg->setCallbacks(new ClockCb(this));

    // GIF_META: Write-with-response
    _charGifMeta = _service->createCharacteristic(
        BLE_CHAR_GIF_META,
        NIMBLE_PROPERTY::WRITE
    );
    _charGifMeta->setCallbacks(new GifMetaCb(this));

    _service->start();

    // ── Advertising ───────────────────────────────────────────────────────────
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);  // helps iPhone connections
    adv->setMaxPreferred(0x12);
    NimBLEDevice::startAdvertising();

    LOG("BLE advertising as '%s'", BLE_DEVICE_NAME);
}

// ── notifyStatus() ────────────────────────────────────────────────────────────

void BleServer::notifyStatus(uint8_t b) {
    if (!_connected || !_charStatus) return;
    _charStatus->setValue(&b, 1);
    _charStatus->notify();
}

// ── Connection callbacks ──────────────────────────────────────────────────────

void BleServer::SrvCb::onConnect(NimBLEServer*) {
    o->_connected = true;
    LOG("BLE client connected");
    o->notifyStatus(STATUS_READY);
}

void BleServer::SrvCb::onDisconnect(NimBLEServer* s) {
    o->_connected = false;
    o->_reset();
    LOG("BLE client disconnected — restarting advertising");
    NimBLEDevice::startAdvertising();
}

// ── FRAME_DATA handler ────────────────────────────────────────────────────────
// Phone writes raw RGB565 chunks here during a transfer.

void BleServer::FrameCb::onWrite(NimBLECharacteristic* c) {
    const uint8_t* data = c->getValue().data();
    size_t         len  = c->getValue().length();
    o->_handleFrameData(data, len);
}

void BleServer::_handleFrameData(const uint8_t* data, size_t len) {
    if (_txState != TransferState::RECEIVING) {
        LOG("WARN: frame data received outside RECEIVING state — ignored");
        return;
    }
    if (!_frameBuf) return;

    // Clamp to buffer bounds
    size_t space = FRAME_BUF_SIZE - _frameBufPos;
    size_t copy  = (len < space) ? len : space;
    memcpy(_frameBuf + _frameBufPos, data, copy);
    _frameBufPos += copy;

    if (copy < len) {
        LOG("WARN: frame buffer overflow — truncated %u bytes",
            (unsigned)(len - copy));
    }
}

// ── CONTROL handler ───────────────────────────────────────────────────────────

void BleServer::CtrlCb::onWrite(NimBLECharacteristic* c) {
    const uint8_t* data = c->getValue().data();
    size_t         len  = c->getValue().length();
    o->_handleControl(data, len);
}

void BleServer::_handleControl(const uint8_t* data, size_t len) {
    if (len == 0) return;
    uint8_t cmd = data[0];

    switch (cmd) {

    // ── CMD_FRAME_BEGIN [0x01, frameIdx, totalFrames] ──────────────────────
    case CMD_FRAME_BEGIN: {
        if (len < 3) { notifyStatus(STATUS_ERROR); return; }
        _txFrameIdx    = data[1];
        _txTotalFrames = data[2];
        _frameBufPos   = 0;
        _txState       = TransferState::RECEIVING;
        LOG("Frame begin: idx=%d total=%d", _txFrameIdx, _txTotalFrames);
        break;
    }

    // ── CMD_FRAME_COMMIT [0x02] ────────────────────────────────────────────
    case CMD_FRAME_COMMIT: {
        if (_txState != TransferState::RECEIVING) {
            notifyStatus(STATUS_ERROR);
            return;
        }
        _txState = TransferState::COMMITTING;
        _commitFrame();
        break;
    }

    // ── CMD_CLEAR [0x03] ──────────────────────────────────────────────────
    case CMD_CLEAR: {
        Display().clear();
        _reset();
        LOG("Display cleared");
        break;
    }

    // ── CMD_SET_MODE [0x04, modeId] ───────────────────────────────────────
    case CMD_SET_MODE: {
        if (len < 2) return;
        _currentMode = data[1];
        LOG("Mode set to %d", _currentMode);

        if (_currentMode == MODE_CLOCK) {
            Clock().startTask();
        } else {
            Clock().stopTask();
        }
        if (_currentMode == MODE_GIF) {
            // GIF playback starts once all frames arrive (see _commitFrame)
        } else {
            Display().stopGifPlayback();
        }
        break;
    }

    // ── CMD_SET_BRIGHTNESS [0x05, value] ─────────────────────────────────
    case CMD_SET_BRIGHT: {
        if (len < 2) return;
        Display().setBrightness(data[1]);
        LOG("Brightness set to %d", data[1]);
        break;
    }

    // ── CMD_ABORT [0x06] ─────────────────────────────────────────────────
    case CMD_ABORT: {
        _reset();
        LOG("Transfer aborted");
        break;
    }

    // ── CMD_PING [0x07] ───────────────────────────────────────────────────
    case CMD_PING: {
        notifyStatus(CMD_PING);   // echo 0x07 back (matches ble_service.dart)
        LOG("Ping");
        break;
    }

    default:
        LOG("Unknown command: 0x%02X", cmd);
        notifyStatus(STATUS_ERROR);
        break;
    }
}

// ── _commitFrame() ────────────────────────────────────────────────────────────

void BleServer::_commitFrame() {
    if (!_frameBuf || _frameBufPos == 0) {
        LOG("Commit: empty buffer — ignored");
        notifyStatus(STATUS_ERROR);
        _txState = TransferState::IDLE;
        return;
    }

    LOG("Commit frame %d/%d  (%u bytes received)",
        _txFrameIdx + 1, _txTotalFrames, (unsigned)_frameBufPos);

    if (_currentMode == MODE_GIF && _txTotalFrames > 1) {
        // Store this frame in the GIF ring buffer
        uint16_t dur = (_txFrameIdx < _gifFrameCount)
                       ? _gifDurations[_txFrameIdx]
                       : 100;  // fallback 100 ms

        bool ok = Display().storeGifFrame(
            _txFrameIdx, _frameBuf, _frameBufPos, dur);

        if (!ok) {
            notifyStatus(STATUS_ERROR);
            _txState = TransferState::IDLE;
            return;
        }

        // If this was the last frame, kick off playback
        if (_txFrameIdx + 1 >= _txTotalFrames) {
            Display().setGifFrameCount(_txTotalFrames);
            Display().startGifPlayback();
            LOG("GIF playback started (%d frames)", _txTotalFrames);
        }

    } else {
        // Still frame — push directly to panel
        Display().showFrame(_frameBuf, _frameBufPos);
    }

    notifyStatus(STATUS_OK);
    _txState = TransferState::IDLE;
}

// ── CLOCK_CONFIG handler ──────────────────────────────────────────────────────

void BleServer::ClockCb::onWrite(NimBLECharacteristic* c) {
    const uint8_t* data = c->getValue().data();
    size_t         len  = c->getValue().length();
    o->_handleClockCfg(data, len);
}

void BleServer::_handleClockCfg(const uint8_t* data, size_t len) {
    // Payload: [epoch u32 big-endian (4 bytes), flags u8]
    // Mirrors BleService.syncClock() in ble_service.dart
    if (len < 5) {
        LOG("CLOCK_CONFIG: payload too short (%u bytes)", (unsigned)len);
        return;
    }

    uint32_t epoch = ((uint32_t)data[0] << 24) |
                     ((uint32_t)data[1] << 16) |
                     ((uint32_t)data[2] <<  8) |
                      (uint32_t)data[3];
    uint8_t flags  = data[4];

    ClockManager::Config cfg;
    cfg.epochUtc    = epoch;
    cfg.is24h       = (flags & 0x01) != 0;
    cfg.showSeconds = (flags & 0x02) != 0;
    cfg.showDate    = (flags & 0x04) != 0;

    Clock().syncTime(cfg);
    LOG("Clock synced: epoch=%lu is24h=%d secs=%d date=%d",
        (unsigned long)epoch, cfg.is24h, cfg.showSeconds, cfg.showDate);
}

// ── GIF_META handler ──────────────────────────────────────────────────────────

void BleServer::GifMetaCb::onWrite(NimBLECharacteristic* c) {
    const uint8_t* data = c->getValue().data();
    size_t         len  = c->getValue().length();
    o->_handleGifMeta(data, len);
}

void BleServer::_handleGifMeta(const uint8_t* data, size_t len) {
    // Payload: [frameCount u8, dur0 u16-BE, dur1 u16-BE, …]
    // Mirrors BleService._sendGifMeta() in ble_service.dart
    if (len < 1) return;
    _gifFrameCount = data[0];
    if (_gifFrameCount > GIF_MAX_FRAMES) _gifFrameCount = GIF_MAX_FRAMES;

    for (uint8_t i = 0; i < _gifFrameCount; i++) {
        size_t offset = 1 + i * 2;
        if (offset + 1 >= len) break;
        _gifDurations[i] = ((uint16_t)data[offset] << 8) | data[offset + 1];
    }
    Display().setGifFrameCount(_gifFrameCount);
    LOG("GIF meta: %d frames", _gifFrameCount);
}

// ── _reset() ─────────────────────────────────────────────────────────────────

void BleServer::_reset() {
    _txState       = TransferState::IDLE;
    _frameBufPos   = 0;
    _txFrameIdx    = 0;
    _txTotalFrames = 1;
}