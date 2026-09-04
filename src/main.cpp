/*
 * Frameon Firmware v4.1 — HID Edition
 * ESP32-S3  ·  P4-2121-64×32 HUB75E
 *
 * v4.1 — Three serial-receive bugs fixed (see FIX notes below).
 *
 * v4.0 — USB composite device: CDC (FRM packets) + HID (controller input).
 *        hidControllerBegin() registers the HID interface with TinyUSB before
 *        Serial.begin() so both interfaces appear in the composite descriptor.
 *
 * Architecture
 * ────────────
 *   Core 0  displayTask — render loop:
 *             1. renderFrame()           blit RGB565 pixels from PSRAM
 *             2. overdrawProgressBar()   Spotify bar from millis()
 *             3. overdrawClock()         live digits from epoch + millis()
 *             4. overdrawPomodoro()      countdown from millis()
 *             5. matrix->flipDMABuffer() commit to display
 *
 *   Core 1  loop()      — non-blocking serial state machine; swaps pending
 *                         buffer into active slot under mutex on valid packet.
 *             inputTask — polls encoder/joystick/buttons at 20 ms; builds
 *                         FrameonHidReport and calls hidControllerSendIfChanged().
 *
 * USB interfaces (composite, no driver install needed on Windows)
 * ───────────────────────────────────────────────────────────────
 *   CDC ACM  → COM port for FRM packet send/receive + ACK/NAK/ERR
 *   HID      → VID 0x303A  PID 0x4001  "Frameon Controller"
 *              8-byte input reports: enc_delta + joy_xy + buttons + events
 *
 * ── FIX v4.1 (three bugs) ────────────────────────────────────────────────────
 *
 *  FIX 1 — Missing Serial.setTxBufferSize(2048).
 *   The setup code called Serial.setRxBufferSize(8192) but omitted the TX side.
 *   The boot log hardcoded "TX buffer: 2048 B" which was a misleading comment,
 *   not a reflection of any actual call.  Without the call the Arduino CDC TX
 *   ring buffer stays at the default (64 B — exactly one USB full-speed packet).
 *   processPacket() prints a 200+ byte DLOGF string before Serial.write(RESP_ACK).
 *   That string fills the tiny ring buffer; the ACK byte must wait for TinyUSB to
 *   drain it.  Under the composite CDC+HID configuration, TinyUSB only gets CPU
 *   time when loop() yields via vTaskDelay, introducing unpredictable latency.
 *   With Serial.setTxBufferSize(2048) the entire DLOGF + ACK fit in one ring-
 *   buffer fill, and Serial.flush() drains them in a single TinyUSB wake-up.
 *
 *  FIX 2 — processSerial() ignored Serial.readBytes() return value.
 *   The payload receive block did:
 *       Serial.readBytes((char*)buf + rxIdx, toRead);
 *       rxIdx += (uint32_t)toRead;          ← always used requested count
 *   Serial.readBytes() returns the number of bytes ACTUALLY read, which can be
 *   less than toRead if the stream timeout fires.  Using toRead unconditionally
 *   advanced rxIdx past un-written memory, so the CRC was computed over garbage,
 *   the firmware sent NAK, and the app retried — only to time out again because
 *   (without FIX 1) the NAK byte also sat unflushed.  Fixed by capturing the
 *   return value and using it for rxIdx.
 *
 *  FIX 3 — processSerial() ran at most once per vTaskDelay(1) tick.
 *   loop() called processSerial() exactly once per millisecond.  Each call
 *   consumed only the bytes currently in the RX ring buffer.  For a 1.2 MB
 *   packet at 921600 baud (~10 s transfer), the 8 KB RX ring fills every ~87 ms.
 *   If processSerial() ever fell behind — e.g. because inputTask preempted loop()
 *   for a HID report — bytes could be silently dropped by the UART/USB driver,
 *   leaving the state machine stuck in RX_PAYLOAD forever and no ACK being sent.
 *   Fixed by wrapping processSerial() in a spin-loop that drains all available
 *   serial data before yielding.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"
#include "hid_controller.h"
#include "clockhelper.h"
#include "pomodorohelper.h"
#include "waitingscreen.h"
#include "inputhelper.h"
#include "debug_log.h"

MatrixPanel_I2S_DMA* matrix = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Double-buffer PSRAM storage
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t* pktBuf[2]  = {nullptr, nullptr};
static uint8_t* nextBuf    = nullptr;
static int      pendingBuf = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Active display state
// Written under swapMutex by Core 1 (processPacket).
// Read under swapMutex by Core 0 (displayTask) — all fields volatile.
// ─────────────────────────────────────────────────────────────────────────────

static SemaphoreHandle_t swapMutex = nullptr;
static SemaphoreHandle_t nextMutex = nullptr;

static volatile int      activeBuf           = 0;
static volatile int      activeFrameCount    = 0;
static volatile uint16_t activeFrameDurMs    = 100;
static volatile uint32_t activeStartPosMs    = 0;
static volatile uint32_t activeTrackDurMs    = 0;
static volatile uint32_t commitTimeMs        = 0;
static volatile uint8_t  activeBarX          = 0;
static volatile uint8_t  activeBarY          = 0;
static volatile uint8_t  activeBarW          = 0;
static volatile uint16_t activeBarColor      = 0x11C5;

static volatile uint8_t  activeClockFlags    = 0;
static volatile uint32_t activeClockEpochSec = 0;
static volatile int16_t  activeClockTzMin    = 0;
static volatile int16_t  activeClockTz2Min   = 0;
static volatile uint8_t  activeClockFontId   = 0;
static volatile int8_t   activeClockOffsetX  = 0;
static volatile int8_t   activeClockOffsetY  = 0;
static volatile uint16_t activeHoursColor    = 0x07E0;
static volatile uint16_t activeMinutesColor  = 0x07E0;
static volatile uint16_t activeSecondsColor  = 0x07E0;
static volatile uint16_t activeColonColor    = 0x07E0;
static volatile uint16_t activeDateColor     = 0x07E0;
static volatile uint16_t activeAmpmColor     = 0x07E0;
static volatile uint8_t  activeClockLayout   = 0;
static volatile uint8_t  activeAnalogFlags   = 0;
static volatile char     activeLabel1[5]     = {0};
static volatile char     activeLabel2[5]     = {0};

static volatile uint8_t  activePomodoroFlags    = 0;
static volatile uint32_t activePomodoroRemSec   = 0;
static volatile uint8_t  activePomodoroPhase    = 0;
static volatile uint8_t  activePomodoroSession  = 0;
static volatile uint8_t  activePomodoroLayout   = 0;
static volatile uint8_t  activePomodoroSessTotal= 4;
static volatile uint16_t activePomodoroTotalSec = 1500;
static volatile int8_t   activePomodoroOffsetX  = 0;
static volatile int8_t   activePomodoroOffsetY  = 0;
static volatile uint16_t activePomodoroColor    = 0xFFE0;

// Next-song preload buffer
static volatile bool     nextBufReady   = false;
static volatile int      nextFrameCount = 0;
static volatile uint16_t nextFrameDurMs = 0;
static volatile uint32_t nextStartPosMs = 0;
static volatile uint32_t nextTrackDurMs = 0;
static volatile uint8_t  nextBarX       = 0;
static volatile uint8_t  nextBarY       = 0;
static volatile uint8_t  nextBarW       = 0;
static volatile uint16_t nextBarColor   = 0x11C5;

// ─────────────────────────────────────────────────────────────────────────────
// Serial receive state machine
// ─────────────────────────────────────────────────────────────────────────────

enum RxState : uint8_t { RX_IDLE, RX_HEADER, RX_PAYLOAD, RX_CRC_H, RX_CRC_L };

static RxState  rxState    = RX_IDLE;
static uint32_t rxIdx      = 0;
static uint8_t  syncBuf[3] = {0, 0, 0};

// Parsed packet fields (Core 1 only — no volatile needed)
static uint8_t  pktFlags          = FRM_VERSION;
static uint16_t pktFrameCount     = 0;
static uint16_t pktWidth          = 0;
static uint16_t pktHeight         = 0;
static uint16_t pktDurMs          = 0;
static uint32_t pktPayloadBytes   = 0;
static uint32_t pktStartPosMs     = 0;
static uint32_t pktTrackDurMs     = 0;
static uint8_t  pktBarX           = 0;
static uint8_t  pktBarY           = 0;
static uint8_t  pktBarW           = 0;
static uint16_t pktBarColor       = 0x11C5;

static uint8_t  pktClockFlags     = 0;
static uint32_t pktClockEpochSec  = 0;
static int16_t  pktClockTzMin     = 0;
static uint8_t  pktClockFontId    = 0;
static int8_t   pktClockOffsetX   = 0;
static int8_t   pktClockOffsetY   = 0;
static uint16_t pktHoursColor     = 0x07E0;
static uint16_t pktMinutesColor   = 0x07E0;
static uint16_t pktSecondsColor   = 0x07E0;
static uint16_t pktColonColor     = 0x07E0;
static uint16_t pktDateColor      = 0x07E0;
static uint16_t pktAmpmColor      = 0x07E0;

static uint8_t  pktClockLayoutStyle = 0;
static uint8_t  pktClockAnalogFlags = 0;
static int16_t  pktClockTz2Min      = 0;
static char     pktClockLabel1[5]   = {0};
static char     pktClockLabel2[5]   = {0};

static uint8_t  pktPomodoroFlags     = 0;
static uint32_t pktPomodoroRemSec    = 0;
static uint8_t  pktPomodoroPhase     = 0;
static uint8_t  pktPomodoroSession   = 0;
static int8_t   pktPomodoroOffsetX   = 0;
static int8_t   pktPomodoroOffsetY   = 0;
static uint16_t pktPomodoroColor     = 0xFFE0;
static uint8_t  pktPomodoroLayout    = 0;
static uint8_t  pktPomodoroSessTotal = 4;
static uint16_t pktPomodoroTotalSec  = 1500;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len);
static void     renderFrame(int bufIdx, int frameIdx);
static void     overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs,
                                    uint8_t barX, uint8_t barY, uint8_t barW,
                                    uint16_t barColor);
static void     parseHeader();
static void     processPacket();
static void     processSerial();
static void     displayTask(void* param);

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16/CCITT — poly 0x1021, init 0xFFFF
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
        }
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHeader — extract all fields from the 80-byte header
// ─────────────────────────────────────────────────────────────────────────────

static void parseHeader() {
    uint8_t* buf = pktBuf[pendingBuf];

    pktFlags        = buf[3];
    pktFrameCount   = ((uint16_t)buf[4]  << 8) | buf[5];
    pktWidth        = ((uint16_t)buf[6]  << 8) | buf[7];
    pktHeight       = ((uint16_t)buf[8]  << 8) | buf[9];
    pktDurMs        = ((uint16_t)buf[10] << 8) | buf[11];
    pktPayloadBytes = ((uint32_t)buf[12] << 24) | ((uint32_t)buf[13] << 16)
                    | ((uint32_t)buf[14] << 8)  |  (uint32_t)buf[15];
    pktStartPosMs   = ((uint32_t)buf[16] << 24) | ((uint32_t)buf[17] << 16)
                    | ((uint32_t)buf[18] << 8)  |  (uint32_t)buf[19];
    pktTrackDurMs   = ((uint32_t)buf[20] << 24) | ((uint32_t)buf[21] << 16)
                    | ((uint32_t)buf[22] << 8)  |  (uint32_t)buf[23];
    pktBarX         = buf[24];
    pktBarY         = buf[25];
    pktBarW         = buf[26];
    pktBarColor     = ((uint16_t)buf[27] << 8) | buf[28];

    pktClockFlags   = buf[29];
    pktClockEpochSec= ((uint32_t)buf[30] << 24) | ((uint32_t)buf[31] << 16)
                    | ((uint32_t)buf[32] << 8)  |  (uint32_t)buf[33];
    pktClockTzMin   = (int16_t)(((uint16_t)buf[34] << 8) | buf[35]);
    pktClockFontId  = buf[36];
    pktClockOffsetX = (int8_t)buf[37];
    pktClockOffsetY = (int8_t)buf[38];
    pktHoursColor   = ((uint16_t)buf[39] << 8) | buf[40];
    pktMinutesColor = ((uint16_t)buf[41] << 8) | buf[42];
    pktSecondsColor = ((uint16_t)buf[43] << 8) | buf[44];
    pktColonColor   = ((uint16_t)buf[45] << 8) | buf[46];
    pktDateColor    = ((uint16_t)buf[47] << 8) | buf[48];
    pktAmpmColor    = ((uint16_t)buf[49] << 8) | buf[50];

    pktClockLayoutStyle = buf[51];
    pktClockAnalogFlags = buf[52];
    pktClockTz2Min      = (int16_t)(((uint16_t)buf[53] << 8) | buf[54]);
    memcpy(pktClockLabel1, &buf[55], 4); pktClockLabel1[4] = '\0';
    memcpy(pktClockLabel2, &buf[59], 4); pktClockLabel2[4] = '\0';

    pktPomodoroFlags     = buf[63];
    pktPomodoroRemSec    = ((uint32_t)buf[64] << 24) | ((uint32_t)buf[65] << 16)
                         | ((uint32_t)buf[66] << 8)  |  (uint32_t)buf[67];
    pktPomodoroPhase     = buf[68];
    pktPomodoroSession   = buf[69];
    pktPomodoroOffsetX   = (int8_t)buf[70];
    pktPomodoroOffsetY   = (int8_t)buf[71];
    pktPomodoroColor     = ((uint16_t)buf[72] << 8) | buf[73];
    pktPomodoroLayout    = buf[74];
    pktPomodoroSessTotal = buf[75];
    pktPomodoroTotalSec  = ((uint16_t)buf[76] << 8) | buf[77];
    // buf[78..79] reserved
}

// ─────────────────────────────────────────────────────────────────────────────
// processPacket — validate CRC, commit or preload, send ACK/NAK
// ─────────────────────────────────────────────────────────────────────────────

static void processPacket() {
    uint8_t* buf = pktBuf[pendingBuf];

    const uint32_t packetLen = HEADER_SIZE + pktPayloadBytes + CRC_SIZE;
    const uint16_t computed  = crc16(buf, HEADER_SIZE + pktPayloadBytes);
    const uint16_t received  = ((uint16_t)buf[HEADER_SIZE + pktPayloadBytes] << 8)
                              |            buf[HEADER_SIZE + pktPayloadBytes + 1];

    if (computed != received) {
        DLOGF("[NAK] CRC mismatch: computed=0x%04X received=0x%04X\n",
              computed, received);
        Serial.flush();          // drain debug text before response byte
        Serial.write(RESP_NAK);
        Serial.flush();          // flush response byte alone
        return;
    }

    // ── Next-song preload ─────────────────────────────────────────────────
    if (pktFlags == FRM_NEXT) {
        xSemaphoreTake(nextMutex, portMAX_DELAY);
        memcpy(nextBuf, buf, packetLen);
        nextFrameCount = pktFrameCount;
        nextFrameDurMs = pktDurMs;
        nextStartPosMs = pktStartPosMs;
        nextTrackDurMs = pktTrackDurMs;
        nextBarX       = pktBarX;
        nextBarY       = pktBarY;
        nextBarW       = pktBarW;
        nextBarColor   = pktBarColor;
        nextBufReady   = true;
        xSemaphoreGive(nextMutex);

        DLOGF("[ACK-NEXT] %d frames preloaded\n", pktFrameCount);
        Serial.flush();          // drain debug text before response byte
        Serial.write(RESP_ACK);
        Serial.flush();          // flush response byte alone
        return;
    }

    // ── Normal commit — update active display state ───────────────────────
    xSemaphoreTake(swapMutex, portMAX_DELAY);

    activeBuf           = pendingBuf;
    activeFrameCount    = pktFrameCount;
    activeFrameDurMs    = pktDurMs ? pktDurMs : 100;
    activeStartPosMs    = pktStartPosMs;
    activeTrackDurMs    = pktTrackDurMs;
    commitTimeMs        = millis();
    activeBarX          = pktBarX;
    activeBarY          = pktBarY;
    activeBarW          = pktBarW;
    activeBarColor      = pktBarColor;

    activeClockFlags    = pktClockFlags;
    activeClockEpochSec = pktClockEpochSec;
    activeClockTzMin    = pktClockTzMin;
    activeClockTz2Min   = pktClockTz2Min;
    activeClockFontId   = pktClockFontId;
    activeClockOffsetX  = pktClockOffsetX;
    activeClockOffsetY  = pktClockOffsetY;
    activeHoursColor    = pktHoursColor;
    activeMinutesColor  = pktMinutesColor;
    activeSecondsColor  = pktSecondsColor;
    activeColonColor    = pktColonColor;
    activeDateColor     = pktDateColor;
    activeAmpmColor     = pktAmpmColor;
    activeClockLayout   = pktClockLayoutStyle;
    activeAnalogFlags   = pktClockAnalogFlags;
    memcpy((void*)activeLabel1, pktClockLabel1, 5);
    memcpy((void*)activeLabel2, pktClockLabel2, 5);

    activePomodoroFlags     = pktPomodoroFlags;
    activePomodoroRemSec    = pktPomodoroRemSec;
    activePomodoroPhase     = pktPomodoroPhase;
    activePomodoroSession   = pktPomodoroSession;
    activePomodoroOffsetX   = pktPomodoroOffsetX;
    activePomodoroOffsetY   = pktPomodoroOffsetY;
    activePomodoroColor     = pktPomodoroColor;
    activePomodoroLayout    = pktPomodoroLayout;
    activePomodoroSessTotal = pktPomodoroSessTotal;
    activePomodoroTotalSec  = pktPomodoroTotalSec;

    pendingBuf ^= 1; // flip double-buffer slot for next receive

    xSemaphoreGive(swapMutex);

    DLOGF("[ACK] flags=0x%02X frames=%d dur=%dms clock=%s font=%d layout=%d pomo=%s\n",
          pktFlags, pktFrameCount, pktDurMs,
          (pktClockFlags & CLK_FLAG_PRESENT) ? "yes" : "no",
          pktClockFontId, pktClockLayoutStyle,
          (pktPomodoroFlags & POMO_FLAG_PRESENT) ? "yes" : "no");

    Serial.flush();          // drain debug text before response byte
    Serial.write(RESP_ACK);
    Serial.flush();          // flush response byte alone
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial — non-blocking receive state machine, Core 1
//
// FIX 2: Serial.readBytes() return value is now used for rxIdx, not toRead.
// FIX 3: Outer while loop drains all available serial data before returning,
//        so no bytes are left in the ring buffer to be dropped if inputTask
//        preempts loop() during the next vTaskDelay tick.
// ─────────────────────────────────────────────────────────────────────────────

static void processSerial() {
    // FIX 3: keep draining as long as there is data and work to do.
    // This prevents the 8 KB RX ring buffer from overflowing when a large
    // packet arrives faster than loop() can call processSerial() at 1 kHz.
    while (Serial.available() || rxState != RX_IDLE) {

        uint8_t* buf = pktBuf[pendingBuf];

        // ── Scan for "FRM" magic ──────────────────────────────────────────
        while (rxState == RX_IDLE && Serial.available()) {
            const uint8_t b = (uint8_t)Serial.read();
            syncBuf[0] = syncBuf[1]; syncBuf[1] = syncBuf[2]; syncBuf[2] = b;
            if (syncBuf[0] == FRM_MAGIC_0 &&
                syncBuf[1] == FRM_MAGIC_1 &&
                syncBuf[2] == FRM_MAGIC_2) {
                buf[0] = FRM_MAGIC_0;
                buf[1] = FRM_MAGIC_1;
                buf[2] = FRM_MAGIC_2;
                rxIdx   = 3;
                rxState = RX_HEADER;
                memset(syncBuf, 0, sizeof(syncBuf));
                DLOGLN("[RX]  Magic found — reading header...");
            }
        }

        // ── Header ────────────────────────────────────────────────────────
        if (rxState == RX_HEADER && Serial.available()) {
            const size_t needed = HEADER_SIZE - rxIdx;
            const size_t avail  = (size_t)Serial.available();
            const size_t toRead = (needed < avail) ? needed : avail;
            // FIX 2: use return value, not toRead, in case of early timeout
            const size_t got    = Serial.readBytes((char*)buf + rxIdx, toRead);
            rxIdx += (uint32_t)got;

            if (rxIdx == HEADER_SIZE) {
                parseHeader();
                const uint32_t expectedPayload =
                    (uint32_t)pktFrameCount * FRAME_BYTES;
                const bool ok =
                    (pktWidth        == PANEL_WIDTH)
                 && (pktHeight       == REAL_HEIGHT)
                 && (pktFrameCount   >  0)
                 && (pktFrameCount   <= MAX_FRAMES)
                 && (pktPayloadBytes == expectedPayload)
                 && (HEADER_SIZE + pktPayloadBytes + CRC_SIZE <= MAX_PACKET)
                 && (pktFlags == FRM_VERSION || pktFlags == FRM_NEXT);

                if (!ok) {
                    DLOGF("[ERR] Header invalid — w=%d h=%d fc=%d flags=0x%02X\n",
                          pktWidth, pktHeight, pktFrameCount, pktFlags);
                    Serial.flush();          // drain debug text before response byte
                    Serial.write(RESP_ERR);
                    Serial.flush();          // flush response byte alone
                    rxState = RX_IDLE;
                    rxIdx   = 0;
                    break; // exit drain loop — wait for next valid packet
                } else {
                    rxState = RX_PAYLOAD;
                    DLOGF("[RX]  Header OK — %d frames %lu B %d ms/frame\n",
                          pktFrameCount, (unsigned long)pktPayloadBytes, pktDurMs);
                }
            }
        }

        // ── Payload ───────────────────────────────────────────────────────
        if (rxState == RX_PAYLOAD && Serial.available()) {
            const uint32_t payloadEnd = HEADER_SIZE + pktPayloadBytes;
            const uint32_t needed     = payloadEnd - rxIdx;
            const size_t   avail      = (size_t)Serial.available();
            const size_t   toRead     = (needed < (uint32_t)avail)
                                        ? (size_t)needed : (size_t)avail;
            // FIX 2: capture actual bytes read — readBytes() may return early
            // if the stream timeout fires before all requested bytes arrive.
            const size_t got = Serial.readBytes((char*)buf + rxIdx, toRead);
            rxIdx += (uint32_t)got;
            if (rxIdx == payloadEnd) rxState = RX_CRC_H;
        }

        // ── CRC ───────────────────────────────────────────────────────────
        if (rxState == RX_CRC_H && Serial.available()) {
            buf[rxIdx++] = (uint8_t)Serial.read();
            rxState = RX_CRC_L;
        }

        if (rxState == RX_CRC_L && Serial.available()) {
            buf[rxIdx++] = (uint8_t)Serial.read();
            processPacket();
            rxState = RX_IDLE;
            rxIdx   = 0;
            break; // packet complete — exit drain loop, yield to other tasks
        }

        // If no progress was made (waiting for more bytes), exit the loop
        // so loop() can call vTaskDelay and let the scheduler run.
        if (!Serial.available()) break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// renderFrame / overdrawProgressBar — Core 0 helpers (unchanged from v4.0)
// ─────────────────────────────────────────────────────────────────────────────

static void renderFrame(int bufIdx, int frameIdx) {
    const uint8_t* src = pktBuf[bufIdx]
                       + HEADER_SIZE
                       + (uint32_t)frameIdx * FRAME_BYTES;
    for (int y = 0; y < REAL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            const int      i     = y * PANEL_WIDTH + x;
            const uint16_t color = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
            matrix->drawPixel(x, y, color); // drawPixel(x, y, rgb565) — Adafruit_GFX API
        }
    }
}

static void overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs,
                                uint8_t barX, uint8_t barY, uint8_t barW,
                                uint16_t barColor) {
    if (trackDurMs == 0 || barW == 0) return;
    float p = (float)songPosMs / (float)trackDurMs;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    const int      filled  = (int)(barW * p + 0.5f);
    const uint16_t bgColor = 0x3186; // #333333 in RGB565
    for (int row = barY; row <= barY + 1; row++) {
        for (int x = 0; x < (int)barW; x++) {
            matrix->drawPixel(barX + x, row, x < filled ? barColor : bgColor);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// displayTask — Core 0 (unchanged from v4.0)
// ─────────────────────────────────────────────────────────────────────────────

static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        xSemaphoreTake(swapMutex, portMAX_DELAY);

        const int      buf        = activeBuf;
        const int      count      = activeFrameCount;
        const uint16_t dur        = activeFrameDurMs;
        const uint32_t startPos   = activeStartPosMs;
        const uint32_t trackDur   = activeTrackDurMs;
        const uint32_t committed  = commitTimeMs;
        const uint8_t  barX       = activeBarX;
        const uint8_t  barY       = activeBarY;
        const uint8_t  barW       = activeBarW;
        const uint16_t barColor   = activeBarColor;
        const uint8_t  clockFlags = activeClockFlags;
        const uint32_t epochSec   = activeClockEpochSec;
        const int16_t  tzMin      = activeClockTzMin;
        const int16_t  tzMin2     = activeClockTz2Min;
        const uint8_t  clockFont  = activeClockFontId;
        const int8_t   clockOffX  = activeClockOffsetX;
        const int8_t   clockOffY  = activeClockOffsetY;
        const uint16_t hoursCol   = activeHoursColor;
        const uint16_t minutesCol = activeMinutesColor;
        const uint16_t secondsCol = activeSecondsColor;
        const uint16_t colonCol   = activeColonColor;
        const uint16_t dateCol    = activeDateColor;
        const uint16_t ampmCol    = activeAmpmColor;
        const uint8_t  clockLayout= activeClockLayout;
        const uint8_t  analogFlags= activeAnalogFlags;
        char label1[5]; memcpy(label1, (const void*)activeLabel1, 5);
        char label2[5]; memcpy(label2, (const void*)activeLabel2, 5);
        const uint8_t  pomoFlags  = activePomodoroFlags;
        const uint32_t pomoRemSec = activePomodoroRemSec;
        const uint8_t  pomoPhase  = activePomodoroPhase;
        const uint8_t  pomoLayout = activePomodoroLayout;
        const uint8_t  pomoSession= activePomodoroSession;
        const uint8_t  pomoSessTotal = activePomodoroSessTotal;
        const uint16_t pomoTotalSec  = activePomodoroTotalSec;
        const int8_t   pomoOffX   = activePomodoroOffsetX;
        const int8_t   pomoOffY   = activePomodoroOffsetY;
        const uint16_t pomoColor  = activePomodoroColor;

        xSemaphoreGive(swapMutex);

        const uint32_t t0 = millis();

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            matrix->flipDMABuffer();
            vTaskDelay(pdMS_TO_TICKS(33)); // ~30 fps while idle
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        renderFrame(buf, currentFrame);

        if (barW > 0 && trackDur > 0) {
            const uint32_t livePos = startPos + (millis() - committed);
            overdrawProgressBar(livePos, trackDur, barX, barY, barW, barColor);
        }

        if (clockFlags & CLK_FLAG_PRESENT) {
            overdrawClock(epochSec, millis() - committed, tzMin, tzMin2,
                          clockFlags, clockLayout, analogFlags, clockFont,
                          clockOffX, clockOffY,
                          hoursCol, minutesCol, secondsCol,
                          colonCol, dateCol, ampmCol, label1, label2);
        }

        if (pomoFlags & POMO_FLAG_PRESENT) {
            overdrawPomodoro(pomoRemSec, pomoTotalSec, millis(),
                             pomoPhase, pomoLayout, pomoFlags,
                             pomoSession, pomoSessTotal,
                             pomoOffX, pomoOffY, pomoColor);
        }

        matrix->flipDMABuffer();

        const uint32_t elapsed   = millis() - t0;
        currentFrame = (currentFrame + 1) % count;
        const int32_t remaining  = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) vTaskDelay(pdMS_TO_TICKS(remaining));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    // ── 1. HID — MUST register before Serial.begin() ─────────────────────
    hidControllerBegin();

    // ── 2. CDC serial — FRM packet bus ────────────────────────────────────
    // FIX 1: Serial.setTxBufferSize() does not exist on the ESP32-S3 USBCDC
    // class — the TinyUSB CDC TX FIFO is a compile-time constant. The runtime
    // fix is to call Serial.flush() BEFORE every Serial.write(RESP_*) so the
    // large DLOGF debug string is fully drained from the TinyUSB TX queue first.
    // The ACK/NAK/ERR byte is then written into an empty buffer and flushed
    // alone, guaranteeing it is transmitted in the very next USB packet.
    // See processPacket() — every Serial.write() is now preceded by flush().
    Serial.setRxBufferSize(8192);
    Serial.begin(921600);
    delay(200);

    BOOTLOGLN("\n\nFrameon Firmware v4.1 (HID edition)");
    BOOTLOGLN("════════════════════════════════════════");
    BOOTLOGF("  USB:         CDC (FRM) + HID (controller)  VID=0x%04X PID=0x%04X\n",
             FRAMEON_USB_VID, FRAMEON_USB_PID);
    BOOTLOGF("  RX buffer:   %u B\n", 8192);

    // ── 3. PSRAM double-buffer ────────────────────────────────────────────
    for (int i = 0; i < 2; i++) {
        pktBuf[i] = (uint8_t*)ps_malloc(MAX_PACKET);
        if (!pktBuf[i]) {
            BOOTLOGF("FATAL: ps_malloc failed for pktBuf[%d]\n", i);
            while (true) { delay(500); BOOTLOG('!'); }
        }
        memset(pktBuf[i], 0, MAX_PACKET);
    }

    nextBuf = (uint8_t*)ps_malloc(MAX_PACKET);
    if (!nextBuf) {
        BOOTLOGLN("FATAL: ps_malloc failed for nextBuf");
        while (true) { delay(500); BOOTLOG('!'); }
    }
    memset(nextBuf, 0, MAX_PACKET);

    BOOTLOGF("PSRAM OK  (3 × %lu KB)   Free: %lu KB\n",
             (unsigned long)(MAX_PACKET / 1024),
             (unsigned long)(ESP.getFreePsram() / 1024));

    // ── 4. FreeRTOS synchronisation ───────────────────────────────────────
    swapMutex = xSemaphoreCreateMutex();
    nextMutex = xSemaphoreCreateMutex();

    // ── 5. LED matrix ────────────────────────────────────────────────────
    BOOTLOGLN("Initialising matrix...");
    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN);
    mxconfig.gpio.r1  = PIN_R1;  mxconfig.gpio.g1  = PIN_G1;
    mxconfig.gpio.b1  = PIN_B1;  mxconfig.gpio.r2  = PIN_R2;
    mxconfig.gpio.g2  = PIN_G2;  mxconfig.gpio.b2  = PIN_B2;
    mxconfig.gpio.a   = PIN_A;   mxconfig.gpio.b   = PIN_B;
    mxconfig.gpio.c   = PIN_C;   mxconfig.gpio.d   = PIN_D;
    mxconfig.gpio.e   = PIN_E;   mxconfig.gpio.clk = PIN_CLK;
    mxconfig.gpio.lat = PIN_LAT; mxconfig.gpio.oe  = PIN_OE;
    mxconfig.driver      = HUB75_I2S_CFG::SHIFTREG;
    mxconfig.clkphase    = false;
    mxconfig.double_buff = true;

    matrix = new MatrixPanel_I2S_DMA(mxconfig);
    if (!matrix->begin()) {
        BOOTLOGLN("FATAL: matrix->begin() failed.");
        while (true) { delay(500); BOOTLOG('.'); }
    }
    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    BOOTLOGLN("Matrix OK.");

    // ── 6. Physical input task (Core 1, priority 1) ───────────────────────
    inputInit();
    inputTaskStart();

    // ── 7. Display task (Core 0, priority 2) ─────────────────────────────
    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 0);

    BOOTLOGLN("Ready.");
    BOOTLOGLN("────────────────────────────────────────");
    BOOTLOGF("  Protocol:   v1.9  (header %d B)\n",  HEADER_SIZE);
    BOOTLOGF("  Panel:      %d × %d px\n",            PANEL_WIDTH, REAL_HEIGHT);
    BOOTLOGF("  Max frames: %d   (%lu KB max payload)\n",
             MAX_FRAMES, (unsigned long)(MAX_PAYLOAD / 1024));
    BOOTLOGLN("  Waiting for Frameon packets on USB Serial...");
    BOOTLOGLN("────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    // FIX 3 is inside processSerial() — it now drains the entire RX ring
    // buffer in one call rather than processing one chunk per tick.
    processSerial();

    InputEventType evt;
    while (xQueueReceive(inputQueue, &evt, 0) == pdTRUE) {
        inputApplyEvent(evt, matrix);
    }

    vTaskDelay(1);
}