/*
 * Frameon Firmware v1.0
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
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"

// ─────────────────────────────────────────────────────────────────────────────
// Matrix
// ─────────────────────────────────────────────────────────────────────────────
static MatrixPanel_I2S_DMA* matrix = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// PSRAM double-buffer
//
// Two packet-sized buffers sit in PSRAM.
// activeBuf   → being rendered by displayTask (core 0)
// pendingBuf  → being filled by the serial receiver (core 1)
// On a successful receive, pendingBuf becomes the new activeBuf atomically
// under swapMutex, then the roles swap.
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
    RX_IDLE,     // scanning for "FRM" magic
    RX_HEADER,   // reading header bytes [3-15]
    RX_PAYLOAD,  // streaming pixel data
    RX_CRC_H,    // reading CRC high byte
    RX_CRC_L,    // reading CRC low byte
};

static RxState   rxState       = RX_IDLE;
static uint32_t  rxIdx         = 0;
static uint8_t   syncBuf[3]    = {0, 0, 0};  // sliding window for magic detection

// Parsed header fields (populated by parseHeader()):
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
static void     showWaitingScreen(uint32_t elapsedMs);
static void     parseHeader();
static void     processPacket();
static void     processSerial();
static void     displayTask(void* param);

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16/CCITT
// Poly: 0x1021   Init: 0xFFFF
// Must match Frameon's FrameExporter._crc16 (frame_exporter.dart).
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
//
// Frame pixel data resides at:
//   pktBuf[bufIdx] + HEADER_SIZE + frameIdx * FRAME_BYTES
//
// Pixels are RGB565, big-endian, row-major (left-to-right, top-to-bottom).
// Only physical rows 0..(REAL_HEIGHT-1) are drawn; the virtual lower half
// (rows 32-63) is left black so the E-pin addressing produces no artefacts.
//
// double_buff = true → we write into the back buffer then flip, giving
// tear-free animation at the cost of one frame of latency.
// ─────────────────────────────────────────────────────────────────────────────
static void renderFrame(int bufIdx, int frameIdx) {
    const uint8_t* src = pktBuf[bufIdx]
                       + HEADER_SIZE
                       + (uint32_t)frameIdx * FRAME_BYTES;

    for (int y = 0; y < REAL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            const int     i     = y * PANEL_WIDTH + x;
            const uint16_t color = ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1];
            matrix->drawPixel(x, y, color);
        }
    }
    matrix->flipDMABuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
// Waiting screen — displayed when no packet has been received yet.
// Shows a minimal Frameon brand mark and a pulsing dot.
// ─────────────────────────────────────────────────────────────────────────────
static void showWaitingScreen(uint32_t elapsedMs) {
    matrix->clearScreen();

    // ── "FRAMEON" — LED green, top-left ──────────────────────────────────
    matrix->setTextSize(1);
    matrix->setTextColor(matrix->color565(33, 195, 44));
    matrix->setCursor(3, 3);
    matrix->print("FRAMEON");

    // ── Thin green underline ──────────────────────────────────────────────
    matrix->drawFastHLine(3, 12, 43, matrix->color565(15, 60, 18));

    // ── "READY" in dim grey ───────────────────────────────────────────────
    matrix->setTextColor(matrix->color565(45, 45, 45));
    matrix->setCursor(3, 19);
    matrix->print("READY");

    // ── Pulsing dot (500 ms period) ───────────────────────────────────────
    bool dotOn = (elapsedMs / 500) % 2 == 0;
    if (dotOn) {
        matrix->fillCircle(57, 22, 2, matrix->color565(33, 195, 44));
    } else {
        matrix->fillCircle(57, 22, 2, matrix->color565(10, 40, 12));
    }

    matrix->flipDMABuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
// Display task — core 0
//
// Loops through the active frame buffer, rendering one frame per
// activeFrameDurMs.  When no frames are loaded, shows the waiting screen.
// The swapMutex is held only for the brief snapshot of volatile state —
// the actual rendering (ms-scale) runs without the mutex.
// ─────────────────────────────────────────────────────────────────────────────
static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        // Snapshot volatile shared state
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

        const uint32_t t0 = millis();
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
// parseHeader — extract fields from pktBuf[pendingBuf][0..15]
// Called after exactly HEADER_SIZE bytes have been received.
// ─────────────────────────────────────────────────────────────────────────────
static void parseHeader() {
    const uint8_t* h = pktBuf[pendingBuf];
    // h[0-2] = magic (already verified)
    // h[3]   = version (accepted as-is for forward compat)
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
// processPacket — validate CRC, check dimensions, commit to active buffer.
// Called after all bytes (header + payload + CRC) have been received into
// pktBuf[pendingBuf].
// ─────────────────────────────────────────────────────────────────────────────
static void processPacket() {
    const uint32_t headerAndPayload = HEADER_SIZE + pktPayloadBytes;

    // ── CRC ───────────────────────────────────────────────────────────────
    const uint16_t storedCrc =
        ((uint16_t)pktBuf[pendingBuf][headerAndPayload] << 8)
        | pktBuf[pendingBuf][headerAndPayload + 1];
    const uint16_t computedCrc = crc16(pktBuf[pendingBuf], headerAndPayload);

    if (storedCrc != computedCrc) {
        Serial.write(RESP_NAK);
        Serial.printf("[NAK] CRC fail — stored=0x%04X computed=0x%04X\n",
                      storedCrc, computedCrc);
        return;
    }

    // ── Dimension & size validation ───────────────────────────────────────
    const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;

    if (pktWidth != PANEL_WIDTH || pktHeight != REAL_HEIGHT) {
        Serial.write(RESP_ERR);
        Serial.printf("[ERR] Wrong dimensions: %dx%d (expected %dx%d)\n",
                      pktWidth, pktHeight, PANEL_WIDTH, REAL_HEIGHT);
        return;
    }
    if (pktFrameCount == 0 || pktFrameCount > MAX_FRAMES) {
        Serial.write(RESP_ERR);
        Serial.printf("[ERR] Frame count out of range: %d (max %d)\n",
                      pktFrameCount, MAX_FRAMES);
        return;
    }
    if (pktPayloadBytes != expectedPayload) {
        Serial.write(RESP_ERR);
        Serial.printf("[ERR] Payload size mismatch: got %lu expected %lu\n",
                      (unsigned long)pktPayloadBytes,
                      (unsigned long)expectedPayload);
        return;
    }

    // ── Commit — swap buffers under mutex ─────────────────────────────────
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    activeBuf        = pendingBuf;
    activeFrameCount = pktFrameCount;
    activeFrameDurMs = (pktDurMs > 0) ? pktDurMs : 100;
    xSemaphoreGive(swapMutex);

    pendingBuf = 1 - pendingBuf;  // next receive goes to the other buffer

    // ── ACK ───────────────────────────────────────────────────────────────
    Serial.write(RESP_ACK);
    Serial.printf("[ACK] %d frames @ %d ms/frame (%.1f fps)  %.1f KB payload\n",
                  pktFrameCount,
                  pktDurMs,
                  pktDurMs > 0 ? 1000.0f / pktDurMs : 0.0f,
                  pktPayloadBytes / 1024.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial — serial receive state machine, called every loop() tick.
//
// State transitions:
//
//   RX_IDLE    sliding-window scan for the 3-byte "FRM" magic sequence.
//              On match, writes magic into pktBuf[pendingBuf][0-2] and
//              advances to RX_HEADER.
//
//   RX_HEADER  reads header bytes [3-15] (13 bytes).
//              On complete, calls parseHeader() and validates the fields.
//              Aborts (→ IDLE + RESP_ERR) if dimensions or sizes are wrong.
//
//   RX_PAYLOAD bulk-reads pixel data directly into pktBuf[pendingBuf].
//              Uses Serial.readBytes() for efficiency on large payloads.
//
//   RX_CRC_H   reads CRC high byte.
//   RX_CRC_L   reads CRC low byte, then calls processPacket().
// ─────────────────────────────────────────────────────────────────────────────
static void processSerial() {
    uint8_t* buf = pktBuf[pendingBuf];

    // ── RX_IDLE: scan for "FRM" using a 3-byte sliding window ────────────
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
            rxIdx  = 3;
            rxState = RX_HEADER;
            memset(syncBuf, 0, sizeof(syncBuf));
            Serial.println("[RX]  Magic found — reading header...");
        }
    }

    // ── RX_HEADER: read bytes [3..15] ────────────────────────────────────
    if (rxState == RX_HEADER && Serial.available()) {
        const size_t needed = HEADER_SIZE - rxIdx;
        const size_t avail  = (size_t)Serial.available();
        const size_t toRead = (needed < avail) ? needed : avail;
        const size_t got    = Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)got;

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
                Serial.write(RESP_ERR);
                Serial.printf("[ERR] Header invalid — w=%d h=%d fc=%d pb=%lu\n",
                              pktWidth, pktHeight, pktFrameCount,
                              (unsigned long)pktPayloadBytes);
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

    // ── RX_PAYLOAD: bulk-read directly into PSRAM buffer ─────────────────
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
    Serial.begin(115200);
    Serial.setTimeout(0);  // readBytes() returns immediately with available data
    delay(500);
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║  Frameon Firmware v1.0               ║");
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

    // SHIFTREG: plain shift-register, no FM6126A / ICN2038S init sequence.
    // PANEL_HEIGHT = 64 (virtual) forces the E address bit to be driven,
    // which is required for 1/32-scan operation on this panel.
    mxconfig.driver      = HUB75_I2S_CFG::SHIFTREG;
    mxconfig.clkphase    = false;
    mxconfig.double_buff = true;   // back-buffer rendering → tear-free frames

    matrix = new MatrixPanel_I2S_DMA(mxconfig);

    if (!matrix->begin()) {
        Serial.println("FATAL: matrix->begin() failed.");
        Serial.println("       Check wiring, GPIO assignments, and power supply.");
        while (true) { delay(500); Serial.print('.'); }
    }

    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    Serial.println("Matrix OK.");

    // ── Spawn display task on core 0 ─────────────────────────────────────
    xTaskCreatePinnedToCore(
        displayTask,  // task function
        "display",    // name
        8192,         // stack (bytes)
        nullptr,      // parameter
        2,            // priority (higher than loop's 1)
        nullptr,      // handle
        0             // core 0
    );

    // ── Print ready summary ───────────────────────────────────────────────
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
// loop — serial receive only.
// Display runs independently on core 0 (displayTask).
// vTaskDelay(1) yields to FreeRTOS scheduler, resets the WDT, and prevents
// this core from starving other tasks during idle periods.
// ─────────────────────────────────────────────────────────────────────────────
void loop() {
    processSerial();
    vTaskDelay(1);
}