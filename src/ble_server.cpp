#include "ble_server.h"
#include "display_manager.h"
#include "clock_manager.h"
#include <time.h>

// ═══════════════════════════════════════════════════════════════════════════
//  ble_server.cpp
//
//  FIX #4: CMD_PING now notifies STATUS_OK (0x00) instead of echoing the
//  command byte (0x07).  The comment in the original code claimed this
//  matched ble_service.dart, but the Dart side checks for STATUS_OK on all
//  successful operations.  If you intentionally need the 0x07 echo for
//  round-trip identification, define a dedicated STATUS_PING byte in
//  config.h and use that instead.
//
//  FIX #6: Frame buffer overflow is no longer silently swallowed.  When
//  incoming data would exceed FRAME_BUF_SIZE the transfer is aborted:
//    • _txState → IDLE
//    • STATUS_ERROR notified to the phone
//    • _reset() called so the next CMD_FRAME_BEGIN starts clean
//  This prevents a truncated buffer being committed as STATUS_OK.
// ═══════════════════════════════════════════════════════════════════════════

// ── Singleton ─────────────────────────────────────────────────────────────────

static BleServer* _instance = nullptr;

BleServer& Ble() {
    if (!_instance) _instance = new BleServer();
    return *_instance;
}

// ── Constructor ───────────────────────────────────────────────────────────────

BleServer::BleServer() {
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
    NimBLEDevice::setMTU(247);

    _server = NimBLEDevice::createServer();
    _server->setCallbacks(new SrvCb(this));

    _service = _server->createService(BLE_SERVICE_UUID);

    _charFrameData = _service->createCharacteristic(
        BLE_CHAR_FRAME_DATA,
        NIMBLE_PROPERTY::WRITE_NR
    );
    _charFrameData->setCallbacks(new FrameCb(this));

    _charControl = _service->createCharacteristic(
        BLE_CHAR_CONTROL,
        NIMBLE_PROPERTY::WRITE
    );
    _charControl->setCallbacks(new CtrlCb(this));

    _charStatus = _service->createCharacteristic(
        BLE_CHAR_STATUS,
        NIMBLE_PROPERTY::NOTIFY
    );

    _charClockCfg = _service->createCharacteristic(
        BLE_CHAR_CLOCK_CONFIG,
        NIMBLE_PROPERTY::WRITE
    );
    _charClockCfg->setCallbacks(new ClockCb(this));

    _charGifMeta = _service->createCharacteristic(
        BLE_CHAR_GIF_META,
        NIMBLE_PROPERTY::WRITE
    );
    _charGifMeta->setCallbacks(new GifMetaCb(this));

    _service->start();

    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->setMinPreferred(0x06);
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

void BleServer::FrameCb::onWrite(NimBLECharacteristic* c) {
    const uint8_t* data = c->getValue().data();
    size_t         len  = c->getValue().length();
    o->_handleFrameData(data, len);
}

// FIX #6: Abort the transfer and notify STATUS_ERROR on overflow instead of
// silently truncating and later committing a corrupt partial frame as OK.

void BleServer::_handleFrameData(const uint8_t* data, size_t len) {
    if (_txState != TransferState::RECEIVING) {
        LOG("WARN: frame data received outside RECEIVING state — ignored");
        return;
    }
    if (!_frameBuf) return;

    size_t space = FRAME_BUF_SIZE - _frameBufPos;

    if (len > space) {
        // FIX #6: Overflow — do NOT copy partial data and silently continue.
        // Abort the transfer so the phone knows something went wrong and can
        // retransmit.  The partial buffer is left as-is but never committed.
        LOG("ERROR: frame buffer overflow (pos=%u, incoming=%u, capacity=%u) — aborting transfer",
            (unsigned)_frameBufPos, (unsigned)len, (unsigned)FRAME_BUF_SIZE);
        notifyStatus(STATUS_ERROR);
        _reset();   // return to IDLE; phone must send CMD_FRAME_BEGIN again
        return;
    }

    memcpy(_frameBuf + _frameBufPos, data, len);
    _frameBufPos += len;
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

    case CMD_FRAME_BEGIN: {
        if (len < 3) { notifyStatus(STATUS_ERROR); return; }
        _txFrameIdx    = data[1];
        _txTotalFrames = data[2];
        _frameBufPos   = 0;
        _txState       = TransferState::RECEIVING;
        LOG("Frame begin: idx=%d total=%d", _txFrameIdx, _txTotalFrames);
        break;
    }

    case CMD_FRAME_COMMIT: {
        if (_txState != TransferState::RECEIVING) {
            notifyStatus(STATUS_ERROR);
            return;
        }
        _txState = TransferState::COMMITTING;
        _commitFrame();
        break;
    }

    case CMD_CLEAR: {
        Display().clear();
        _reset();
        LOG("Display cleared");
        break;
    }

    case CMD_SET_MODE: {
        if (len < 2) return;
        _currentMode = data[1];
        LOG("Mode set to %d", _currentMode);

        if (_currentMode == MODE_CLOCK) {
            Clock().startTask();
        } else {
            Clock().stopTask();
        }
        if (_currentMode != MODE_GIF) {
            Display().stopGifPlayback();
        }
        break;
    }

    case CMD_SET_BRIGHT: {
        if (len < 2) return;
        Display().setBrightness(data[1]);
        LOG("Brightness set to %d", data[1]);
        break;
    }

    case CMD_ABORT: {
        _reset();
        LOG("Transfer aborted");
        break;
    }

    // FIX #4: Respond with STATUS_OK, not the raw command byte 0x07.
    // The Flutter ble_service.dart checks the status characteristic for
    // STATUS_OK (0x00) to confirm successful operations.  Echoing 0x07
    // would cause the ping handler on the Dart side to treat the response
    // as an error unless it explicitly special-cases the ping byte.
    // If a distinct ping echo is needed, add STATUS_PONG to config.h.
    case CMD_PING: {
        notifyStatus(STATUS_OK);
        LOG("Ping → STATUS_OK");
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
        uint16_t dur = (_txFrameIdx < _gifFrameCount)
                       ? _gifDurations[_txFrameIdx]
                       : 100;

        bool ok = Display().storeGifFrame(
            _txFrameIdx, _frameBuf, _frameBufPos, dur);

        if (!ok) {
            notifyStatus(STATUS_ERROR);
            _txState = TransferState::IDLE;
            return;
        }

        if (_txFrameIdx + 1 >= _txTotalFrames) {
            Display().setGifFrameCount(_txTotalFrames);
            Display().startGifPlayback();
            LOG("GIF playback started (%d frames)", _txTotalFrames);
        }

    } else {
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