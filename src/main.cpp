/*
 * Frameon Firmware v1.1
 * ESP32-S3-N16R8  ·  P4-2121-64×32 HUB75E
 *
 * Receives pre-rendered RGB565 frame packets from the Frameon desktop app
 * over USB Serial, validates them, and plays them back on the LED matrix
 * in a seamless loop — independent of further host communication.
 *
 * Architecture
 * ────────────
 *   Core 0  displayTask  — continuously renders the active frame buffer to the
 *                          matrix, advancing one frame per frameDurationMs.
 *
 *   Core 1  loop()       — runs the serial receive state machine; when a
 *                          complete, valid packet arrives it swaps the pending
 *                          buffer into the active slot under a mutex.
 *
 * PSRAM usage (2 × ~1.2 MB double buffer)
 * ─────────────────────────────────────────
 *   pktBuf[0], pktBuf[1]: alternating receive / display buffers.
 *   Layout of each buffer:
 *     [ header 16 B ][ frame_0 4096 B ][ frame_1 ]…[ frame_N-1 ][ crc 2 B ]
 *   Frames are read directly from the buffer — no extra copy needed.
 *
 * Protocol (matches Frameon's FrameExporter — frame_exporter.dart)
 * ──────────────────────────────────────────────────────────────────
 *   Header (16 B, all multi-byte fields big-endian):
 *     [0-2]   "FRM" magic
 *     [3]     version 0x01
 *     [4-5]   frame count  (uint16 BE)
 *     [6-7]   width        (uint16 BE)  must be 64
 *     [8-9]   height       (uint16 BE)  must be 32
 *     [10-11] duration ms  (uint16 BE)
 *     [12-15] payload bytes (uint32 BE)
 *   Payload: frame_count × 64 × 32 × 2 bytes  (RGB565 BE, row-major)
 *   CRC:     CRC-16/CCITT over header + payload  (poly=0x1021, init=0xFFFF)
 *
 * Serial responses:
 *   0x06 ACK — valid packet committed
 *   0x15 NAK — CRC mismatch
 *   0x1B ERR — malformed header / unsupported dimensions
 *
 * FIX v1.1 — Serial.flush() after every Serial.write() response byte
 * ────────────────────────────────────────────────────────────────────
 *   ESP32-S3 native USB CDC (TinyUSB) buffers outgoing data and waits to
 *   fill a 64-byte USB packet before transmitting. A single-byte response
 *   (ACK/NAK/ERR) would sit in the TinyUSB TX buffer indefinitely without
 *   an explicit flush, causing the Frameon app to time out after 15 s with
 *   "No response from device". Serial.flush() forces immediate transmission.
 *
 * Response byte ordering rule (unchanged from v1.0):
 *   Serial.printf() (debug text) must always be sent BEFORE Serial.write()
 *   (the response byte). Flutter's readResponseByte() polls for the first
 *   available byte; if debug text arrives first it reads '[' (0x5B) instead
 *   of the ACK/NAK/ERR byte and throws "Unexpected response byte: 0x5B".
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"
#include "waitingscreen.h"

// ─────────────────────────────────────────────────────────────────────────────
// Matrix
// ─────────────────────────────────────────────────────────────────────────────

MatrixPanel_I2S_DMA* matrix = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// PSRAM double-buffer
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t*          pktBuf[2]          = {nullptr, nullptr};
static volatile int      activeBuf          = 0;
static int               pendingBuf         = 1;
static volatile int      activeFrameCount   = 0;
static volatile uint16_t activeFrameDurMs   = 100;

static SemaphoreHandle_t swapMutex          = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Serial receive state machine
// ─────────────────────────────────────────────────────────────────────────────
enum RxState : uint8_t {
    RX_IDLE,
    RX_HEADER,
    RX_PAYLOAD,
    RX_CRC_H,
    RX_CRC_L,
};

static RxState   rxState       = RX_IDLE;
static uint32_t  rxIdx         = 0;
static uint8_t   syncBuf[3]    = {0, 0, 0};

static uint16_t  pktFrameCount   = 0;
static uint16_t  pktWidth        = 0;
static uint16_t  pktHeight       = 0;
static uint16_t  pktDurMs        = 0;
static uint32_t  pktPayloadBytes = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
static uint16_t crc16(const uint8_t* data, size_t len);
static void     renderFrame(int bufIdx, int frameIdx);
static void     parseHeader();
static void     processPacket();
static void     processSerial();
static void     displayTask(void* param);

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16/CCITT
// Poly: 0x1021   Init: 0xFFFF
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
// Render one frame to the matrix
// ─────────────────────────────────────────────────────────────────────────────
static void renderFrame(int bufIdx, int frameIdx) {
    const uint8_t* src = pktBuf[bufIdx]
                       + HEADER_SIZE
                       + (uint32_t)frameIdx * FRAME_BYTES;

    for (int y = 0; y < REAL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            const int      i     = y * PANEL_WIDTH + x;
            const uint16_t color = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
            matrix->drawPixel(x, y, color);
        }
    }
    matrix->flipDMABuffer();
}



// ─────────────────────────────────────────────────────────────────────────────
// Display task — Core 0
//
// Continuously renders the active frame buffer to the LED matrix.
// Reads activeBuf / activeFrameCount / activeFrameDurMs under the swap mutex
// so it always sees a consistent snapshot even if Core 1 swaps buffers
// mid-render.
// ─────────────────────────────────────────────────────────────────────────────
static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        xSemaphoreTake(swapMutex, portMAX_DELAY);
        const int      buf   = activeBuf;
        const int      count = activeFrameCount;
        const uint16_t dur   = activeFrameDurMs;
        xSemaphoreGive(swapMutex);

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        const uint32_t t0      = millis();
        renderFrame(buf, currentFrame);
        const uint32_t elapsed = millis() - t0;

        currentFrame = (currentFrame + 1) % count;

        const int32_t remaining = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) {
            vTaskDelay(pdMS_TO_TICKS(remaining));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHeader — extract fields from the 16-byte header in pktBuf[pendingBuf]
// ─────────────────────────────────────────────────────────────────────────────
static void parseHeader() {
    const uint8_t* h = pktBuf[pendingBuf];
    pktFrameCount   = ((uint16_t)h[4]  << 8) | h[5];
    pktWidth        = ((uint16_t)h[6]  << 8) | h[7];
    pktHeight       = ((uint16_t)h[8]  << 8) | h[9];
    pktDurMs        = ((uint16_t)h[10] << 8) | h[11];
    pktPayloadBytes = ((uint32_t)h[12] << 24)
                    | ((uint32_t)h[13] << 16)
                    | ((uint32_t)h[14] <<  8)
                    |            h[15];
}

// ─────────────────────────────────────────────────────────────────────────────
// processPacket — validate CRC + dimensions, commit buffer, send response
//
// FIX v1.1: Serial.flush() is called after every Serial.write() response byte.
//
// Why: ESP32-S3 native USB CDC uses TinyUSB internally. TinyUSB accumulates
// outgoing bytes in a 64-byte endpoint buffer and only transmits when the
// buffer is full OR an explicit flush is requested. A single-byte ACK/NAK/ERR
// would never reach the host on its own — it would sit in TinyUSB's TX buffer
// until more data arrived. Serial.flush() forces an immediate USB bulk transfer
// so the Frameon app receives the response byte within milliseconds instead of
// timing out after 15 s.
//
// Rule: Serial.printf() debug text BEFORE Serial.write() response byte,
// then Serial.flush() to push the byte to the host immediately.
// ─────────────────────────────────────────────────────────────────────────────
static void processPacket() {
    const uint32_t headerAndPayload = HEADER_SIZE + pktPayloadBytes;

    // ── CRC validation ────────────────────────────────────────────────────
    const uint16_t storedCrc =
        ((uint16_t)pktBuf[pendingBuf][headerAndPayload] << 8)
        | pktBuf[pendingBuf][headerAndPayload + 1];
    const uint16_t computedCrc = crc16(pktBuf[pendingBuf], headerAndPayload);

    if (storedCrc != computedCrc) {
        Serial.printf("[NAK] CRC fail — stored=0x%04X computed=0x%04X\n",
                      storedCrc, computedCrc);
        Serial.write(RESP_NAK);
        Serial.flush(); // FIX: force TinyUSB to transmit the NAK byte immediately
        return;
    }

    // ── Dimension validation ──────────────────────────────────────────────
    const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;

    if (pktWidth != PANEL_WIDTH || pktHeight != REAL_HEIGHT) {
        Serial.printf("[ERR] Wrong dimensions: %dx%d (expected %dx%d)\n",
                      pktWidth, pktHeight, PANEL_WIDTH, REAL_HEIGHT);
        Serial.write(RESP_ERR);
        Serial.flush(); // FIX: force TinyUSB to transmit the ERR byte immediately
        return;
    }

    if (pktFrameCount == 0 || pktFrameCount > MAX_FRAMES) {
        Serial.printf("[ERR] Frame count out of range: %d (max %d)\n",
                      pktFrameCount, MAX_FRAMES);
        Serial.write(RESP_ERR);
        Serial.flush(); // FIX: force TinyUSB to transmit the ERR byte immediately
        return;
    }

    if (pktPayloadBytes != expectedPayload) {
        Serial.printf("[ERR] Payload size mismatch: got %lu expected %lu\n",
                      (unsigned long)pktPayloadBytes,
                      (unsigned long)expectedPayload);
        Serial.write(RESP_ERR);
        Serial.flush(); // FIX: force TinyUSB to transmit the ERR byte immediately
        return;
    }

    // ── Commit — swap buffers under mutex ─────────────────────────────────
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    activeBuf        = pendingBuf;
    activeFrameCount = pktFrameCount;
    activeFrameDurMs = (pktDurMs > 0) ? pktDurMs : 100;
    xSemaphoreGive(swapMutex);

    pendingBuf = 1 - pendingBuf;

    // ── ACK — debug text FIRST, then response byte, then flush ────────────
    Serial.printf("[ACK] %d frames @ %d ms/frame (%.1f fps)  %.1f KB payload\n",
                  pktFrameCount,
                  pktDurMs,
                  pktDurMs > 0 ? 1000.0f / pktDurMs : 0.0f,
                  pktPayloadBytes / 1024.0f);
    Serial.write(RESP_ACK);
    Serial.flush(); // FIX: force TinyUSB to transmit the ACK byte immediately
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial — serial receive state machine, called from loop()
//
// Runs entirely on Core 1. Reads available bytes from the USB CDC Serial
// buffer and advances through the packet receive states one chunk at a time.
// When a complete packet is assembled (magic + header + payload + CRC),
// calls processPacket() to validate and commit it.
// ─────────────────────────────────────────────────────────────────────────────
static void processSerial() {
    uint8_t* buf = pktBuf[pendingBuf];

    // ── RX_IDLE: scan for "FRM" magic ─────────────────────────────────────
    while (rxState == RX_IDLE && Serial.available()) {
        const uint8_t b = (uint8_t)Serial.read();
        syncBuf[0] = syncBuf[1];
        syncBuf[1] = syncBuf[2];
        syncBuf[2] = b;

        if (syncBuf[0] == FRM_MAGIC_0 &&
            syncBuf[1] == FRM_MAGIC_1 &&
            syncBuf[2] == FRM_MAGIC_2) {
            buf[0] = FRM_MAGIC_0;
            buf[1] = FRM_MAGIC_1;
            buf[2] = FRM_MAGIC_2;
            rxIdx   = 3;
            rxState = RX_HEADER;
            memset(syncBuf, 0, sizeof(syncBuf));
            Serial.println("[RX]  Magic found — reading header...");
        }
    }

    // ── RX_HEADER ─────────────────────────────────────────────────────────
    if (rxState == RX_HEADER && Serial.available()) {
        const size_t needed = HEADER_SIZE - rxIdx;
        const size_t avail  = (size_t)Serial.available();
        const size_t toRead = (needed < avail) ? needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;

        if (rxIdx == HEADER_SIZE) {
            parseHeader();

            const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;
            const bool ok = (pktWidth        == PANEL_WIDTH)
                         && (pktHeight       == REAL_HEIGHT)
                         && (pktFrameCount   >  0)
                         && (pktFrameCount   <= MAX_FRAMES)
                         && (pktPayloadBytes == expectedPayload)
                         && (HEADER_SIZE + pktPayloadBytes + CRC_SIZE <= MAX_PACKET);

            if (!ok) {
                Serial.printf("[ERR] Header invalid — w=%d h=%d fc=%d pb=%lu\n",
                              pktWidth, pktHeight, pktFrameCount,
                              (unsigned long)pktPayloadBytes);
                Serial.write(RESP_ERR);
                Serial.flush(); // FIX: force TinyUSB to transmit the ERR byte immediately
                rxState = RX_IDLE;
                rxIdx   = 0;
            } else {
                rxState = RX_PAYLOAD;
                Serial.printf("[RX]  Header OK — %d frames, %lu B payload, %d ms/frame\n",
                              pktFrameCount,
                              (unsigned long)pktPayloadBytes,
                              pktDurMs);
            }
        }
    }

    // ── RX_PAYLOAD ────────────────────────────────────────────────────────
    if (rxState == RX_PAYLOAD && Serial.available()) {
        const uint32_t payloadEnd = HEADER_SIZE + pktPayloadBytes;
        const uint32_t needed     = payloadEnd - rxIdx;
        const size_t   avail      = (size_t)Serial.available();
        const size_t   toRead     = (needed < (uint32_t)avail)
                                    ? (size_t)needed
                                    : avail;
        const size_t   got        = Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)got;

        if (rxIdx >= payloadEnd) {
            rxState = RX_CRC_H;
            Serial.println("[RX]  Payload received — reading CRC...");
        }
    }

    // ── RX_CRC_H / RX_CRC_L ──────────────────────────────────────────────
    if (rxState == RX_CRC_H && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        rxState = RX_CRC_L;
    }
    if (rxState == RX_CRC_L && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        processPacket();
        rxState = RX_IDLE;
        rxIdx   = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(921600);
    Serial.setTimeout(0);
    delay(500);

    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║  Frameon Firmware v1.1               ║");
    Serial.println("║  ESP32-S3-N16R8  ·  HUB75E 64×32     ║");
    Serial.println("╚══════════════════════════════════════╝");

    // ── PSRAM double-buffer allocation ────────────────────────────────────
    Serial.printf("Allocating 2 × %lu KB in PSRAM...\n",
                  (unsigned long)(MAX_PACKET / 1024));

    for (int i = 0; i < 2; i++) {
        pktBuf[i] = (uint8_t*)ps_malloc(MAX_PACKET);
        if (!pktBuf[i]) {
            Serial.printf("FATAL: ps_malloc failed for pktBuf[%d]\n", i);
            Serial.printf("       Requested: %lu bytes\n", (unsigned long)MAX_PACKET);
            Serial.printf("       Free PSRAM: %lu bytes\n",
                          (unsigned long)ESP.getFreePsram());
            while (true) { delay(500); Serial.print('!'); }
        }
        memset(pktBuf[i], 0, MAX_PACKET);
    }

    Serial.printf("PSRAM buffers OK.  Free: %lu KB\n",
                  (unsigned long)(ESP.getFreePsram() / 1024));

    // ── FreeRTOS mutex ────────────────────────────────────────────────────
    swapMutex = xSemaphoreCreateMutex();

    // ── Matrix initialisation ─────────────────────────────────────────────
    Serial.println("Initialising matrix...");

    HUB75_I2S_CFG mxconfig(PANEL_WIDTH, PANEL_HEIGHT, PANEL_CHAIN);

    mxconfig.gpio.r1  = PIN_R1;
    mxconfig.gpio.g1  = PIN_G1;
    mxconfig.gpio.b1  = PIN_B1;
    mxconfig.gpio.r2  = PIN_R2;
    mxconfig.gpio.g2  = PIN_G2;
    mxconfig.gpio.b2  = PIN_B2;
    mxconfig.gpio.a   = PIN_A;
    mxconfig.gpio.b   = PIN_B;
    mxconfig.gpio.c   = PIN_C;
    mxconfig.gpio.d   = PIN_D;
    mxconfig.gpio.e   = PIN_E;
    mxconfig.gpio.clk = PIN_CLK;
    mxconfig.gpio.lat = PIN_LAT;
    mxconfig.gpio.oe  = PIN_OE;

    mxconfig.driver      = HUB75_I2S_CFG::SHIFTREG;
    mxconfig.clkphase    = false;
    mxconfig.double_buff = true;

    matrix = new MatrixPanel_I2S_DMA(mxconfig);

    if (!matrix->begin()) {
        Serial.println("FATAL: matrix->begin() failed.");
        while (true) { delay(500); Serial.print('.'); }
    }

    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    Serial.println("Matrix OK.");

    // ── Spawn display task on Core 0 ──────────────────────────────────────
    xTaskCreatePinnedToCore(
        displayTask,
        "display",
        8192,
        nullptr,
        2,
        nullptr,
        0
    );

    Serial.println("Ready.");
    Serial.println("────────────────────────────────────────");
    Serial.printf( "  Panel:       %dx%d  (virtual %dx%d)\n",
                   PANEL_WIDTH, REAL_HEIGHT, PANEL_WIDTH, PANEL_HEIGHT);
    Serial.printf( "  Max frames:  %d  (%lu KB max payload)\n",
                   MAX_FRAMES, (unsigned long)(MAX_PAYLOAD / 1024));
    Serial.printf( "  Frame size:  %d B  (%dx%d × RGB565)\n",
                   FRAME_BYTES, PANEL_WIDTH, REAL_HEIGHT);
    Serial.println("  Waiting for Frameon packets on USB Serial...");
    Serial.println("────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop — Core 1
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    processSerial();
    vTaskDelay(1); // yield to FreeRTOS scheduler; keeps watchdog happy
}