/*
 * Frameon Firmware v1.4
 * ESP32-S3-N16R8  ·  P4-2121-64×32 HUB75E
 *
 * v1.2 additions
 * ──────────────
 * 1. PROGRESS-BAR PREDICTION
 *    The header now carries startPositionMs + trackDurationMs (8 new bytes).
 *    displayTask tracks millis() from the moment a packet is committed and
 *    repaints the progress bar row on every frame using real elapsed time —
 *    not the looping frame index. The bar stays pixel-accurate between
 *    app re-syncs, independent of loop length.
 *
 * 2. NEXT-SONG PRELOAD
 *    A second pending buffer (nextBuf) stores a queued next-song packet sent
 *    by the app ~10 s before the current track ends (version byte 0x4E).
 *    displayTask swaps the preloaded packet in automatically when the
 *    predicted song position reaches (trackDurationMs - 10000 ms), giving a
 *    seamless track transition with no visible gap.
 *
 * Protocol header v1.4 (30 bytes):
 *   [0-2]   "FRM" magic
 *   [3]     0x02 = normal  |  0x4E = next-song preload
 *   [4-5]   frame count    (uint16 BE)
 *   [6-7]   width          (uint16 BE)
 *   [8-9]   height         (uint16 BE)
 *   [10-11] frame dur ms   (uint16 BE)
 *   [12-15] payload bytes  (uint32 BE)
 *   [16-19] startPositionMs(uint32 BE)
 *   [20-23] trackDurationMs(uint32 BE)
 *   [24]    barX           (uint8)   — progress bar left edge
 *   [25]    barY           (uint8)   — progress bar top edge
 *   [26]    barW           (uint8)   — progress bar width (0 = no bar)
 *   [27-28] barColor       (uint16 BE RGB565) — filled bar color
 *   [29]    reserved       (uint8)   — 0x00
 *   [30..]  RGB565 payload
 *   [-2..]  CRC-16/CCITT
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
// PSRAM buffers
//
// pktBuf[0/1]  — double-buffer for currently-displaying / incoming packet
// nextBuf      — preloaded next-song packet (queued with 0x4E flag)
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t*          pktBuf[2]          = {nullptr, nullptr};
static uint8_t*          nextBuf            = nullptr;  // next-song preload
static volatile int      activeBuf          = 0;
static int               pendingBuf         = 1;

static volatile int      activeFrameCount   = 0;
static volatile uint16_t activeFrameDurMs   = 100;

// Progress-bar prediction state (written under swapMutex, read on Core 0)
static volatile uint32_t activeStartPosMs   = 0;  // song pos when packet committed
static volatile uint32_t activeTrackDurMs   = 0;  // total song duration (0 = no bar)
static volatile uint32_t commitTimeMs       = 0;  // millis() when packet committed
// Bar geometry — per-layout from header (v1.4)
static volatile uint8_t  activeBarX         = 0;      // bar left edge
static volatile uint8_t  activeBarY         = 0;      // bar top edge
static volatile uint8_t  activeBarW         = 0;      // bar width (0 = no bar)
static volatile uint16_t activeBarColor     = 0x11C5; // bar fg color RGB565

// Next-song preload state
static volatile bool     nextBufReady       = false;  // nextBuf holds valid packet
static volatile int      nextFrameCount     = 0;
static volatile uint16_t nextFrameDurMs     = 100;
static volatile uint32_t nextStartPosMs     = 0;
static volatile uint32_t nextTrackDurMs     = 0;
static volatile uint8_t  nextBarX           = 0;
static volatile uint8_t  nextBarY           = 0;
static volatile uint8_t  nextBarW           = 0;
static volatile uint16_t nextBarColor       = 0x11C5;

static SemaphoreHandle_t swapMutex          = nullptr;
static SemaphoreHandle_t nextMutex          = nullptr;  // guards nextBuf fields

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

static uint8_t   pktFlags        = FRM_VERSION;
static uint16_t  pktFrameCount   = 0;
static uint16_t  pktWidth        = 0;
static uint16_t  pktHeight       = 0;
static uint16_t  pktDurMs        = 0;
static uint32_t  pktPayloadBytes = 0;
static uint32_t  pktStartPosMs   = 0;
static uint32_t  pktTrackDurMs   = 0;
static uint8_t   pktBarX         = 0;
static uint8_t   pktBarY         = 0;
static uint8_t   pktBarW         = 0;
static uint16_t  pktBarColor     = 0x11C5;

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len);
static void     renderFrame(int bufIdx, int frameIdx);
static void     overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs, uint8_t barX, uint8_t barY, uint8_t barW, uint16_t barColor);
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
// renderFrame — blit one frame from the packet buffer to the matrix
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
    // NOTE: flipDMABuffer() is intentionally NOT called here.
    // displayTask calls it exactly once after renderFrame() + overdrawProgressBar()
    // so both draws land in the same buffer swap. Calling flip twice would
    // swap back to the old frame, erasing the progress bar overdraw.
}

// ─────────────────────────────────────────────────────────────────────────────
// overdrawProgressBar
//
// Repaints the 2-pixel-tall progress bar using the predicted song position
// computed from millis(). Bar geometry (barX, barY, barW) comes from the
// packet header (v1.3) and exactly matches what spotify_widget.dart baked:
//
//   artAndText : barX=33  barY=29  barW=30   (right of art)
//   textOnly   : barX=0   barY=30  barW=63   (full width)
//   artOnly    : barW=0   → function returns immediately, art untouched
//
// This prevents the v1.2 bug where the firmware painted over album art.
// ─────────────────────────────────────────────────────────────────────────────

static void overdrawProgressBar(uint32_t songPosMs, uint32_t trackDurMs,
                                 uint8_t barX, uint8_t barY, uint8_t barW,
                                 uint16_t barColor) {
    if (trackDurMs == 0 || barW == 0) return;

    float p = (float)songPosMs / (float)trackDurMs;
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;

    const int filled = (int)(barW * p + 0.5f);

    // Background: #333333 → RGB565 0x3186
    const uint16_t bgColor = 0x3186;
    // Foreground: from packet header — set by layer.progressColor in Dart
    const uint16_t fgColor = barColor;

    for (int row = barY; row <= barY + 1; row++) {
        for (int x = 0; x < barW; x++) {
            matrix->drawPixel(barX + x, row, x < filled ? fgColor : bgColor);
        }
    }
    // NOTE: flipDMABuffer() is called by displayTask after this returns.
}

// ─────────────────────────────────────────────────────────────────────────────
// Display task — Core 0
// ─────────────────────────────────────────────────────────────────────────────

static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        // ── Snapshot active buffer state ──────────────────────────────────
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
        xSemaphoreGive(swapMutex);

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        // ── Predict current song position ─────────────────────────────────
        // elapsed since commit is wall-clock time regardless of loop wraps.
        const uint32_t now        = millis();
        const uint32_t wallMs     = now - committed;
        const uint32_t songPosMs  = startPos + wallMs;

        // ── Check if next-song preload should activate ────────────────────
        // Swap in the preloaded next-song buffer 10 s before track end.
        if (trackDur > 0 && songPosMs >= trackDur - 10000UL) {
            xSemaphoreTake(nextMutex, portMAX_DELAY);
            const bool hasNext = nextBufReady;
            xSemaphoreGive(nextMutex);

            if (hasNext) {
                // Copy next-song data into the pending packet buffer and commit.
                xSemaphoreTake(nextMutex, portMAX_DELAY);
                memcpy(pktBuf[pendingBuf], nextBuf, MAX_PACKET);
                const int      nCount  = nextFrameCount;
                const uint16_t nDur    = nextFrameDurMs;
                const uint32_t nStart  = nextStartPosMs;
                const uint32_t nTrack  = nextTrackDurMs;
                // bar geometry for next song
                nextBarX = nextBarX; nextBarY = nextBarY; nextBarW = nextBarW;
                nextBufReady = false;
                xSemaphoreGive(nextMutex);

                xSemaphoreTake(swapMutex, portMAX_DELAY);
                activeBuf        = pendingBuf;
                activeFrameCount = nCount;
                activeFrameDurMs = nDur;
                activeStartPosMs = nStart;
                activeTrackDurMs = nTrack;
                commitTimeMs     = millis();
                activeBarX       = nextBarX;
                activeBarY       = nextBarY;
                activeBarW       = nextBarW;
                activeBarColor   = nextBarColor;
                xSemaphoreGive(swapMutex);

                pendingBuf   = 1 - pendingBuf;
                currentFrame = 0;

                Serial.println("[NEXT] Next-song preload activated.");
                continue; // re-read the new active state at top of loop
            }
        }

        // ── Render current frame ──────────────────────────────────────────
        const uint32_t t0 = millis();
        renderFrame(buf, currentFrame);          // draws pixels, no flip yet

        // ── Overdraw progress bar with predicted real-time position ───────
        // barW == 0 → returns immediately (artOnly or showProgress off)
        overdrawProgressBar(songPosMs, trackDur, barX, barY, barW, barColor);

        // ── Single flip — commits both frame pixels + bar overdraw together ─
        // Flipping once here ensures the full composited image appears
        // atomically. Previously renderFrame and overdrawProgressBar each
        // called flipDMABuffer(), causing the second flip to swap back to the
        // old frame and erase everything drawn by the first flip.
        matrix->flipDMABuffer();

        const uint32_t elapsed = millis() - t0;
        currentFrame = (currentFrame + 1) % count;

        const int32_t remaining = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) {
            vTaskDelay(pdMS_TO_TICKS(remaining));
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHeader — read v1.2 fields from pktBuf[pendingBuf]
// ─────────────────────────────────────────────────────────────────────────────

static void parseHeader() {
    const uint8_t* h = pktBuf[pendingBuf];
    pktFlags        =  h[3];
    pktFrameCount   = ((uint16_t)h[4]  << 8) | h[5];
    pktWidth        = ((uint16_t)h[6]  << 8) | h[7];
    pktHeight       = ((uint16_t)h[8]  << 8) | h[9];
    pktDurMs        = ((uint16_t)h[10] << 8) | h[11];
    pktPayloadBytes = ((uint32_t)h[12] << 24)
                    | ((uint32_t)h[13] << 16)
                    | ((uint32_t)h[14] <<  8)
                    |            h[15];
    pktStartPosMs   = ((uint32_t)h[16] << 24)
                    | ((uint32_t)h[17] << 16)
                    | ((uint32_t)h[18] <<  8)
                    |            h[19];
    pktTrackDurMs   = ((uint32_t)h[20] << 24)
                    | ((uint32_t)h[21] << 16)
                    | ((uint32_t)h[22] <<  8)
                    |            h[23];
    pktBarX         = h[24];
    pktBarY         = h[25];
    pktBarW         = h[26];
    pktBarColor     = ((uint16_t)h[27] << 8) | h[28];
    // h[29] reserved
}

// ─────────────────────────────────────────────────────────────────────────────
// processPacket — validate CRC, commit or queue next-song
// ─────────────────────────────────────────────────────────────────────────────

static void processPacket() {
    const uint32_t headerAndPayload = HEADER_SIZE + pktPayloadBytes;

    // ── CRC ───────────────────────────────────────────────────────────────
    const uint16_t storedCrc =
        ((uint16_t)pktBuf[pendingBuf][headerAndPayload] << 8)
        | pktBuf[pendingBuf][headerAndPayload + 1];
    const uint16_t computedCrc = crc16(pktBuf[pendingBuf], headerAndPayload);

    if (storedCrc != computedCrc) {
        Serial.printf("[NAK] CRC fail — stored=0x%04X computed=0x%04X\n",
                      storedCrc, computedCrc);
        Serial.write(RESP_NAK);
        Serial.flush();
        return;
    }

    // ── Dimension validation ──────────────────────────────────────────────
    const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;

    if (pktWidth != PANEL_WIDTH || pktHeight != REAL_HEIGHT ||
        pktFrameCount == 0 || pktFrameCount > MAX_FRAMES ||
        pktPayloadBytes != expectedPayload) {
        Serial.printf("[ERR] Invalid packet — w=%d h=%d fc=%d pb=%lu\n",
                      pktWidth, pktHeight, pktFrameCount,
                      (unsigned long)pktPayloadBytes);
        Serial.write(RESP_ERR);
        Serial.flush();
        return;
    }

    // ── Route: normal commit vs next-song preload ─────────────────────────
    if (pktFlags == FRM_NEXT) {
        // Store in nextBuf — displayTask will swap it in at the right time.
        xSemaphoreTake(nextMutex, portMAX_DELAY);
        memcpy(nextBuf, pktBuf[pendingBuf], HEADER_SIZE + pktPayloadBytes + CRC_SIZE);
        nextFrameCount = pktFrameCount;
        nextFrameDurMs = (pktDurMs > 0) ? pktDurMs : 100;
        nextStartPosMs = pktStartPosMs;
        nextTrackDurMs = pktTrackDurMs;
        nextBarX       = pktBarX;
        nextBarY       = pktBarY;
        nextBarW       = pktBarW;
        nextBarColor   = pktBarColor;
        nextBufReady   = true;
        xSemaphoreGive(nextMutex);

        Serial.printf("[NEXT] Preloaded next-song: %d frames, startPos=%lu ms, trackDur=%lu ms\n",
                      pktFrameCount,
                      (unsigned long)pktStartPosMs,
                      (unsigned long)pktTrackDurMs);
        Serial.write(RESP_ACK);
        Serial.flush();
        // pendingBuf is NOT rotated — we copied to nextBuf, not consumed pendingBuf.
        return;
    }

    // Normal commit — swap into active slot.
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    activeBuf        = pendingBuf;
    activeFrameCount = pktFrameCount;
    activeFrameDurMs = (pktDurMs > 0) ? pktDurMs : 100;
    activeStartPosMs = pktStartPosMs;
    activeTrackDurMs = pktTrackDurMs;
    commitTimeMs     = millis();
    activeBarX       = pktBarX;
    activeBarY       = pktBarY;
    activeBarW       = pktBarW;
    activeBarColor   = pktBarColor;
    xSemaphoreGive(swapMutex);

    pendingBuf = 1 - pendingBuf;

    Serial.printf("[ACK] %d frames @ %d ms/frame  startPos=%lu ms  trackDur=%lu ms  bar=(%d,%d,%d)\n",
                  pktFrameCount, pktDurMs,
                  (unsigned long)pktStartPosMs,
                  (unsigned long)pktTrackDurMs,
                  pktBarX, pktBarY, pktBarW);
    Serial.write(RESP_ACK);
    Serial.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial — serial receive state machine, Core 1
// ─────────────────────────────────────────────────────────────────────────────

static void processSerial() {
    // For next-song packets we receive into pendingBuf then memcpy to nextBuf.
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
                         && (HEADER_SIZE + pktPayloadBytes + CRC_SIZE <= MAX_PACKET)
                         && (pktFlags == FRM_VERSION || pktFlags == FRM_NEXT);

            if (!ok) {
                Serial.printf("[ERR] Header invalid — flags=0x%02X w=%d h=%d fc=%d pb=%lu\n",
                              pktFlags, pktWidth, pktHeight, pktFrameCount,
                              (unsigned long)pktPayloadBytes);
                Serial.write(RESP_ERR);
                Serial.flush();
                rxState = RX_IDLE;
                rxIdx   = 0;
            } else {
                rxState = RX_PAYLOAD;
                Serial.printf("[RX]  Header OK — flags=0x%02X %d frames %lu B %d ms/frame startPos=%lu trackDur=%lu\n",
                              pktFlags, pktFrameCount,
                              (unsigned long)pktPayloadBytes, pktDurMs,
                              (unsigned long)pktStartPosMs,
                              (unsigned long)pktTrackDurMs);
            }
        }
    }

    // ── RX_PAYLOAD ────────────────────────────────────────────────────────
    if (rxState == RX_PAYLOAD && Serial.available()) {
        const uint32_t payloadEnd = HEADER_SIZE + pktPayloadBytes;
        const uint32_t needed     = payloadEnd - rxIdx;
        const size_t   avail      = (size_t)Serial.available();
        const size_t   toRead     = (needed < (uint32_t)avail)
                                    ? (size_t)needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;
        if (rxIdx == payloadEnd) {
            rxState = RX_CRC_H;
        }
    }

    // ── RX_CRC_H ─────────────────────────────────────────────────────────
    if (rxState == RX_CRC_H && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        rxState = RX_CRC_L;
    }

    // ── RX_CRC_L ─────────────────────────────────────────────────────────
    if (rxState == RX_CRC_L && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        processPacket();
        rxState = RX_IDLE;
        rxIdx   = 0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(921600);
    delay(200);
    Serial.println("\n\nFrameon Firmware v1.4");
    Serial.println("════════════════════════════════════════");

    // ── PSRAM allocation ──────────────────────────────────────────────────
    for (int i = 0; i < 2; i++) {
        pktBuf[i] = (uint8_t*)ps_malloc(MAX_PACKET);
        if (!pktBuf[i]) {
            Serial.printf("FATAL: ps_malloc failed for pktBuf[%d]\n", i);
            while (true) { delay(500); Serial.print('!'); }
        }
        memset(pktBuf[i], 0, MAX_PACKET);
    }

    // nextBuf: same size as a regular packet buffer
    nextBuf = (uint8_t*)ps_malloc(MAX_PACKET);
    if (!nextBuf) {
        Serial.println("FATAL: ps_malloc failed for nextBuf");
        while (true) { delay(500); Serial.print('!'); }
    }
    memset(nextBuf, 0, MAX_PACKET);

    Serial.printf("PSRAM buffers OK (3 × %lu KB).  Free: %lu KB\n",
                  (unsigned long)(MAX_PACKET / 1024),
                  (unsigned long)(ESP.getFreePsram() / 1024));

    // ── Mutexes ───────────────────────────────────────────────────────────
    swapMutex = xSemaphoreCreateMutex();
    nextMutex = xSemaphoreCreateMutex();

    // ── Matrix ────────────────────────────────────────────────────────────
    Serial.println("Initialising matrix...");

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
        Serial.println("FATAL: matrix->begin() failed.");
        while (true) { delay(500); Serial.print('.'); }
    }
    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    Serial.println("Matrix OK.");

    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 0);

    Serial.println("Ready.");
    Serial.println("────────────────────────────────────────");
    Serial.printf("  Protocol:    v1.4 (header %d B)\n", HEADER_SIZE);
    Serial.printf("  Panel:       %dx%d\n", PANEL_WIDTH, REAL_HEIGHT);
    Serial.printf("  Max frames:  %d  (%lu KB max payload)\n",
                  MAX_FRAMES, (unsigned long)(MAX_PAYLOAD / 1024));
    Serial.println("  Waiting for Frameon packets on USB Serial...");
    Serial.println("────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    processSerial();
    vTaskDelay(1);
}