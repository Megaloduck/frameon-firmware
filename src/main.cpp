/*
 * Frameon Firmware v1.5
 * ESP32-S3-N16R8  ·  P4-2121-64×32 HUB75E
 *
 * v1.5 — Clock overdraw (clockhelper.cpp)
 * v1.4 — Per-layer progress bar colour
 * v1.2 — Progress-bar prediction + next-song preload
 *
 * Header layout v1.5 (52 bytes, all multi-byte fields big-endian):
 *   [0-2]   "FRM" magic
 *   [3]     0x02 normal  |  0x4E next-song preload
 *   [4-5]   frameCount   uint16
 *   [6-7]   width        uint16
 *   [8-9]   height       uint16
 *   [10-11] durMs        uint16
 *   [12-15] payloadBytes uint32
 *   [16-19] startPosMs   uint32
 *   [20-23] trackDurMs   uint32
 *   [24]    barX         uint8
 *   [25]    barY         uint8
 *   [26]    barW         uint8
 *   [27-28] barColor     uint16 RGB565
 *   [29]    clockFlags   uint8  CLK_FLAG_* bitmask
 *   [30-33] clockEpochSec uint32
 *   [34-35] clockTzMin   int16
 *   [36]    clockFontId  uint8
 *   [37]    clockOffsetX int8
 *   [38]    clockOffsetY int8
 *   [39]    reserved     uint8
 *   [40-41] hoursColor   uint16 RGB565
 *   [42-43] minutesColor uint16 RGB565
 *   [44-45] secondsColor uint16 RGB565
 *   [46-47] colonColor   uint16 RGB565
 *   [48-49] dateColor    uint16 RGB565
 *   [50-51] ampmColor    uint16 RGB565
 *   [52..N] RGB565 pixel data
 *   [N+1-2] CRC-16/CCITT
 */

#include <Arduino.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "frameon.h"
#include "waitingscreen.h"
#include "spotifyhelper.h"
#include "clockhelper.h"

// ─────────────────────────────────────────────────────────────────────────────
// Matrix
// ─────────────────────────────────────────────────────────────────────────────

MatrixPanel_I2S_DMA* matrix = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// PSRAM buffers
// ─────────────────────────────────────────────────────────────────────────────

static uint8_t*  pktBuf[2] = {nullptr, nullptr};
static uint8_t*  nextBuf   = nullptr;
static volatile int activeBuf  = 0;
static int          pendingBuf = 1;

// ─────────────────────────────────────────────────────────────────────────────
// Active display state
// ─────────────────────────────────────────────────────────────────────────────

static volatile int      activeFrameCount  = 0;
static volatile uint16_t activeFrameDurMs  = 100;
static volatile uint32_t activeStartPosMs  = 0;
static volatile uint32_t activeTrackDurMs  = 0;
static volatile uint32_t commitTimeMs      = 0;
static volatile uint8_t  activeBarX        = 0;
static volatile uint8_t  activeBarY        = 0;
static volatile uint8_t  activeBarW        = 0;
static volatile uint16_t activeBarColor    = 0x11C5;

// v1.5 clock state
static volatile uint8_t  activeClockFlags    = 0;
static volatile uint32_t activeClockEpochSec = 0;
static volatile int16_t  activeClockTzMin    = 0;
static volatile uint8_t  activeClockFontId   = 0;
static volatile int8_t   activeClockOffsetX  = 0;
static volatile int8_t   activeClockOffsetY  = 0;
static volatile uint16_t activeHoursColor    = 0x07E0;
static volatile uint16_t activeMinutesColor  = 0x07E0;
static volatile uint16_t activeSecondsColor  = 0x07E0;
static volatile uint16_t activeColonColor    = 0x07E0;
static volatile uint16_t activeDateColor     = 0x07E0;
static volatile uint16_t activeAmpmColor     = 0x07E0;

// next-song preload state
static volatile bool     nextBufReady   = false;
static volatile int      nextFrameCount = 0;
static volatile uint16_t nextFrameDurMs = 100;
static volatile uint32_t nextStartPosMs = 0;
static volatile uint32_t nextTrackDurMs = 0;
static volatile uint8_t  nextBarX       = 0;
static volatile uint8_t  nextBarY       = 0;
static volatile uint8_t  nextBarW       = 0;
static volatile uint16_t nextBarColor   = 0x11C5;

static SemaphoreHandle_t swapMutex = nullptr;
static SemaphoreHandle_t nextMutex = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Serial receive state machine
// ─────────────────────────────────────────────────────────────────────────────

enum RxState : uint8_t { RX_IDLE, RX_HEADER, RX_PAYLOAD, RX_CRC_H, RX_CRC_L };

static RxState  rxState    = RX_IDLE;
static uint32_t rxIdx      = 0;
static uint8_t  syncBuf[3] = {0, 0, 0};

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
static uint8_t   pktClockFlags    = 0;
static uint32_t  pktClockEpochSec = 0;
static int16_t   pktClockTzMin    = 0;
static uint8_t   pktClockFontId   = 0;
static int8_t    pktClockOffsetX  = 0;
static int8_t    pktClockOffsetY  = 0;
static uint16_t  pktHoursColor    = 0x07E0;
static uint16_t  pktMinutesColor  = 0x07E0;
static uint16_t  pktSecondsColor  = 0x07E0;
static uint16_t  pktColonColor    = 0x07E0;
static uint16_t  pktDateColor     = 0x07E0;
static uint16_t  pktAmpmColor     = 0x07E0;

// ─────────────────────────────────────────────────────────────────────────────
// CRC-16/CCITT
// ─────────────────────────────────────────────────────────────────────────────

static uint16_t crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) : (crc << 1);
    }
    return crc;
}

// ─────────────────────────────────────────────────────────────────────────────
// renderFrame
// ─────────────────────────────────────────────────────────────────────────────

static void renderFrame(int bufIdx, int frameIdx) {
    const uint8_t* src = pktBuf[bufIdx]
                       + HEADER_SIZE
                       + (uint32_t)frameIdx * FRAME_BYTES;
    for (int y = 0; y < REAL_HEIGHT; y++) {
        for (int x = 0; x < PANEL_WIDTH; x++) {
            const int i = y * PANEL_WIDTH + x;
            matrix->drawPixel(x, y,
                ((uint16_t)src[i * 2] << 8) | src[i * 2 + 1]);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// parseHeader — v1.5 (52-byte header)
// ─────────────────────────────────────────────────────────────────────────────

static void parseHeader() {
    const uint8_t* h = pktBuf[pendingBuf];
    pktFlags        =  h[3];
    pktFrameCount   = ((uint16_t)h[4]  << 8) | h[5];
    pktWidth        = ((uint16_t)h[6]  << 8) | h[7];
    pktHeight       = ((uint16_t)h[8]  << 8) | h[9];
    pktDurMs        = ((uint16_t)h[10] << 8) | h[11];
    pktPayloadBytes = ((uint32_t)h[12] << 24) | ((uint32_t)h[13] << 16)
                    | ((uint32_t)h[14] <<  8) |            h[15];
    pktStartPosMs   = ((uint32_t)h[16] << 24) | ((uint32_t)h[17] << 16)
                    | ((uint32_t)h[18] <<  8) |            h[19];
    pktTrackDurMs   = ((uint32_t)h[20] << 24) | ((uint32_t)h[21] << 16)
                    | ((uint32_t)h[22] <<  8) |            h[23];
    pktBarX         = h[24];
    pktBarY         = h[25];
    pktBarW         = h[26];
    pktBarColor     = ((uint16_t)h[27] << 8) | h[28];
    pktClockFlags    = h[29];
    pktClockEpochSec = ((uint32_t)h[30] << 24) | ((uint32_t)h[31] << 16)
                     | ((uint32_t)h[32] <<  8) |            h[33];
    pktClockTzMin    = (int16_t)(((uint16_t)h[34] << 8) | h[35]);
    pktClockFontId   = h[36];
    pktClockOffsetX  = (int8_t)h[37];
    pktClockOffsetY  = (int8_t)h[38];
    // h[39] reserved
    pktHoursColor    = ((uint16_t)h[40] << 8) | h[41];
    pktMinutesColor  = ((uint16_t)h[42] << 8) | h[43];
    pktSecondsColor  = ((uint16_t)h[44] << 8) | h[45];
    pktColonColor    = ((uint16_t)h[46] << 8) | h[47];
    pktDateColor     = ((uint16_t)h[48] << 8) | h[49];
    pktAmpmColor     = ((uint16_t)h[50] << 8) | h[51];
}

// ─────────────────────────────────────────────────────────────────────────────
// processPacket
// ─────────────────────────────────────────────────────────────────────────────

static void processPacket() {
    const uint32_t headerAndPayload = HEADER_SIZE + pktPayloadBytes;
    const uint16_t storedCrc =
        ((uint16_t)pktBuf[pendingBuf][headerAndPayload] << 8)
        | pktBuf[pendingBuf][headerAndPayload + 1];
    const uint16_t computedCrc = crc16(pktBuf[pendingBuf], headerAndPayload);

    if (storedCrc != computedCrc) {
        Serial.printf("[NAK] CRC fail stored=0x%04X computed=0x%04X\n",
                      storedCrc, computedCrc);
        Serial.write(RESP_NAK); Serial.flush(); return;
    }

    const uint32_t expectedPayload = (uint32_t)pktFrameCount * FRAME_BYTES;
    if (pktWidth != PANEL_WIDTH || pktHeight != REAL_HEIGHT ||
        pktFrameCount == 0 || pktFrameCount > MAX_FRAMES ||
        pktPayloadBytes != expectedPayload) {
        Serial.printf("[ERR] Invalid w=%d h=%d fc=%d pb=%lu\n",
                      pktWidth, pktHeight, pktFrameCount,
                      (unsigned long)pktPayloadBytes);
        Serial.write(RESP_ERR); Serial.flush(); return;
    }

    if (pktFlags == FRM_NEXT) {
        xSemaphoreTake(nextMutex, portMAX_DELAY);
        memcpy(nextBuf, pktBuf[pendingBuf],
               HEADER_SIZE + pktPayloadBytes + CRC_SIZE);
        nextFrameCount = pktFrameCount;
        nextFrameDurMs = pktDurMs > 0 ? pktDurMs : 100;
        nextStartPosMs = pktStartPosMs;
        nextTrackDurMs = pktTrackDurMs;
        nextBarX = pktBarX; nextBarY = pktBarY;
        nextBarW = pktBarW; nextBarColor = pktBarColor;
        nextBufReady = true;
        xSemaphoreGive(nextMutex);
        Serial.printf("[NEXT] Preloaded %d frames\n", pktFrameCount);
        Serial.write(RESP_ACK); Serial.flush(); return;
    }

    // Normal commit
    xSemaphoreTake(swapMutex, portMAX_DELAY);
    activeBuf          = pendingBuf;
    activeFrameCount   = pktFrameCount;
    activeFrameDurMs   = pktDurMs > 0 ? pktDurMs : 100;
    activeStartPosMs   = pktStartPosMs;
    activeTrackDurMs   = pktTrackDurMs;
    commitTimeMs       = millis();
    activeBarX         = pktBarX;
    activeBarY         = pktBarY;
    activeBarW         = pktBarW;
    activeBarColor     = pktBarColor;
    activeClockFlags   = pktClockFlags;
    activeClockEpochSec= pktClockEpochSec;
    activeClockTzMin   = pktClockTzMin;
    activeClockFontId  = pktClockFontId;
    activeClockOffsetX = pktClockOffsetX;
    activeClockOffsetY = pktClockOffsetY;
    activeHoursColor   = pktHoursColor;
    activeMinutesColor = pktMinutesColor;
    activeSecondsColor = pktSecondsColor;
    activeColonColor   = pktColonColor;
    activeDateColor    = pktDateColor;
    activeAmpmColor    = pktAmpmColor;
    xSemaphoreGive(swapMutex);

    pendingBuf = 1 - pendingBuf;

    Serial.printf("[ACK] %d frames @ %dms  clock=%s epoch=%lu tzMin=%d\n",
                  pktFrameCount, pktDurMs,
                  (pktClockFlags & CLK_FLAG_PRESENT) ? "yes" : "no",
                  (unsigned long)pktClockEpochSec, (int)pktClockTzMin);
    Serial.write(RESP_ACK); Serial.flush();
}

// ─────────────────────────────────────────────────────────────────────────────
// processSerial
// ─────────────────────────────────────────────────────────────────────────────

static void processSerial() {
    uint8_t* buf = pktBuf[pendingBuf];

    while (rxState == RX_IDLE && Serial.available()) {
        const uint8_t b = (uint8_t)Serial.read();
        syncBuf[0] = syncBuf[1]; syncBuf[1] = syncBuf[2]; syncBuf[2] = b;
        if (syncBuf[0]==FRM_MAGIC_0 && syncBuf[1]==FRM_MAGIC_1 && syncBuf[2]==FRM_MAGIC_2) {
            buf[0]=FRM_MAGIC_0; buf[1]=FRM_MAGIC_1; buf[2]=FRM_MAGIC_2;
            rxIdx=3; rxState=RX_HEADER;
            memset(syncBuf,0,sizeof(syncBuf));
            Serial.println("[RX]  Magic found");
        }
    }

    if (rxState == RX_HEADER && Serial.available()) {
        const size_t needed = HEADER_SIZE - rxIdx;
        const size_t avail  = (size_t)Serial.available();
        const size_t toRead = needed < avail ? needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;
        if (rxIdx == HEADER_SIZE) {
            parseHeader();
            const uint32_t expPay = (uint32_t)pktFrameCount * FRAME_BYTES;
            const bool ok = (pktWidth==PANEL_WIDTH) && (pktHeight==REAL_HEIGHT)
                         && (pktFrameCount>0) && (pktFrameCount<=MAX_FRAMES)
                         && (pktPayloadBytes==expPay)
                         && (HEADER_SIZE+pktPayloadBytes+CRC_SIZE<=MAX_PACKET)
                         && (pktFlags==FRM_VERSION || pktFlags==FRM_NEXT);
            if (!ok) {
                Serial.printf("[ERR] Header invalid fl=0x%02X w=%d h=%d fc=%d\n",
                              pktFlags, pktWidth, pktHeight, pktFrameCount);
                Serial.write(RESP_ERR); Serial.flush();
                rxState=RX_IDLE; rxIdx=0;
            } else {
                rxState=RX_PAYLOAD;
                Serial.printf("[RX]  Header OK %d frames %lu B %dms\n",
                              pktFrameCount,(unsigned long)pktPayloadBytes,pktDurMs);
            }
        }
    }

    if (rxState == RX_PAYLOAD && Serial.available()) {
        const uint32_t payloadEnd = HEADER_SIZE + pktPayloadBytes;
        const uint32_t needed     = payloadEnd - rxIdx;
        const size_t   avail      = (size_t)Serial.available();
        const size_t   toRead     = needed < (uint32_t)avail ? needed : avail;
        Serial.readBytes((char*)buf + rxIdx, toRead);
        rxIdx += (uint32_t)toRead;
        if (rxIdx == payloadEnd) rxState = RX_CRC_H;
    }

    if (rxState == RX_CRC_H && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        rxState = RX_CRC_L;
    }
    if (rxState == RX_CRC_L && Serial.available()) {
        buf[rxIdx++] = (uint8_t)Serial.read();
        processPacket();
        rxState=RX_IDLE; rxIdx=0;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// displayTask — Core 0
// ─────────────────────────────────────────────────────────────────────────────

static void displayTask(void* /*param*/) {
    int      currentFrame = 0;
    uint32_t taskStart    = millis();

    while (true) {
        xSemaphoreTake(swapMutex, portMAX_DELAY);
        const int      buf      = activeBuf;
        const int      count    = activeFrameCount;
        const uint16_t dur      = activeFrameDurMs;
        const uint32_t startPos = activeStartPosMs;
        const uint32_t trackDur = activeTrackDurMs;
        const uint32_t committed= commitTimeMs;
        const uint8_t  barX     = activeBarX;
        const uint8_t  barY     = activeBarY;
        const uint8_t  barW     = activeBarW;
        const uint16_t barColor = activeBarColor;
        const uint8_t  clkFlags = activeClockFlags;
        const uint32_t clkEpoch = activeClockEpochSec;
        const int16_t  clkTzMin = activeClockTzMin;
        const uint8_t  clkFont  = activeClockFontId;
        const int8_t   clkOffX  = activeClockOffsetX;
        const int8_t   clkOffY  = activeClockOffsetY;
        const uint16_t clkHours = activeHoursColor;
        const uint16_t clkMins  = activeMinutesColor;
        const uint16_t clkSecs  = activeSecondsColor;
        const uint16_t clkColon = activeColonColor;
        const uint16_t clkDate  = activeDateColor;
        const uint16_t clkAmpm  = activeAmpmColor;
        xSemaphoreGive(swapMutex);

        if (count == 0) {
            showWaitingScreen(millis() - taskStart);
            matrix->flipDMABuffer();
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        if (currentFrame >= count) currentFrame = 0;

        const uint32_t now       = millis();
        const uint32_t wallMs    = now - committed;
        const uint32_t songPosMs = startPos + wallMs;

        // Next-song preload swap
        if (trackDur > 0 && songPosMs >= trackDur - 10000UL) {
            xSemaphoreTake(nextMutex, portMAX_DELAY);
            const bool hasNext = nextBufReady;
            xSemaphoreGive(nextMutex);
            if (hasNext) {
                xSemaphoreTake(nextMutex, portMAX_DELAY);
                memcpy(pktBuf[pendingBuf], nextBuf, MAX_PACKET);
                const int      nCount = nextFrameCount;
                const uint16_t nDur   = nextFrameDurMs;
                const uint32_t nStart = nextStartPosMs;
                const uint32_t nTrack = nextTrackDurMs;
                const uint8_t  nBarX  = nextBarX;
                const uint8_t  nBarY  = nextBarY;
                const uint8_t  nBarW  = nextBarW;
                const uint16_t nBarC  = nextBarColor;
                nextBufReady = false;
                xSemaphoreGive(nextMutex);
                xSemaphoreTake(swapMutex, portMAX_DELAY);
                activeBuf        = pendingBuf;
                activeFrameCount = nCount;
                activeFrameDurMs = nDur;
                activeStartPosMs = nStart;
                activeTrackDurMs = nTrack;
                commitTimeMs     = millis();
                activeBarX=nBarX; activeBarY=nBarY;
                activeBarW=nBarW; activeBarColor=nBarC;
                xSemaphoreGive(swapMutex);
                pendingBuf=1-pendingBuf; currentFrame=0;
                Serial.println("[NEXT] Activated.");
                continue;
            }
        }

        const uint32_t t0 = millis();

        // 1. Background frame
        renderFrame(buf, currentFrame);

        // 2. Spotify progress bar (spotifyhelper.cpp)
        overdrawProgressBar(songPosMs, trackDur, barX, barY, barW, barColor);

        // 3. Clock overdraw (clockhelper.cpp)
        if (clkFlags & CLK_FLAG_PRESENT) {
            overdrawClock(clkEpoch, wallMs, clkTzMin, clkFlags,
                          clkFont, clkOffX, clkOffY,
                          clkHours, clkMins, clkSecs,
                          clkColon, clkDate, clkAmpm);
        }

        // 4. Single atomic flip
        matrix->flipDMABuffer();

        const uint32_t elapsed  = millis() - t0;
        currentFrame = (currentFrame + 1) % count;
        const int32_t remaining = (int32_t)dur - (int32_t)elapsed;
        if (remaining > 1) vTaskDelay(pdMS_TO_TICKS(remaining));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// setup — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(921600);
    delay(200);
    Serial.println("\n\nFrameon Firmware v1.5");
    Serial.println("════════════════════════════════════════");

    for (int i = 0; i < 2; i++) {
        pktBuf[i] = (uint8_t*)ps_malloc(MAX_PACKET);
        if (!pktBuf[i]) {
            Serial.printf("FATAL: ps_malloc failed pktBuf[%d]\n", i);
            while (true) { delay(500); Serial.print('!'); }
        }
        memset(pktBuf[i], 0, MAX_PACKET);
    }

    nextBuf = (uint8_t*)ps_malloc(MAX_PACKET);
    if (!nextBuf) {
        Serial.println("FATAL: ps_malloc failed nextBuf");
        while (true) { delay(500); Serial.print('!'); }
    }
    memset(nextBuf, 0, MAX_PACKET);

    Serial.printf("PSRAM OK (3 x %lu KB)  Free: %lu KB\n",
                  (unsigned long)(MAX_PACKET/1024),
                  (unsigned long)(ESP.getFreePsram()/1024));

    swapMutex = xSemaphoreCreateMutex();
    nextMutex = xSemaphoreCreateMutex();

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
        Serial.println("FATAL: matrix->begin() failed");
        while (true) { delay(500); Serial.print('.'); }
    }
    matrix->setBrightness8(DEFAULT_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
    Serial.println("Matrix OK.");

    xTaskCreatePinnedToCore(displayTask, "display", 8192, nullptr, 2, nullptr, 0);

    Serial.println("Ready.");
    Serial.println("────────────────────────────────────────");
    Serial.printf("  Protocol: v1.5 (header %d B)\n", HEADER_SIZE);
    Serial.printf("  Panel:    %dx%d\n", PANEL_WIDTH, REAL_HEIGHT);
    Serial.printf("  MaxFrames:%d (%lu KB max)\n",
                  MAX_FRAMES, (unsigned long)(MAX_PAYLOAD/1024));
    Serial.println("  Waiting for packets...");
    Serial.println("────────────────────────────────────────");
}

// ─────────────────────────────────────────────────────────────────────────────
// loop — Core 1
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    processSerial();
    vTaskDelay(1);
}