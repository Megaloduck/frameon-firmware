#include <Arduino.h>
#include "config.h"
#include "display_manager.h"
#include "ble_server.h"
#include "clock_manager.h"

// ═══════════════════════════════════════════════════════════════════════════
//  main.cpp  —  Frameon ESP32-S3 firmware
//
//  Boot sequence:
//    1. Serial (debug logging)
//    2. PSRAM check
//    3. HUB75 display initialisation
//    4. BLE server (begins advertising immediately)
//    5. Display FreeRTOS task (owns GIF playback timing)
//
//  All ongoing work runs in FreeRTOS tasks pinned to core 1.
//  BLE / NimBLE uses core 0.  loop() is essentially idle.
// ═══════════════════════════════════════════════════════════════════════════

// Forward declaration
static void showSplash();

void setup() {
    Serial.begin(115200);
    delay(500); // allow USB CDC to attach on ESP32-S3

    LOG("============================================");
    LOG(" Frameon firmware  (build: " __DATE__ " " __TIME__ ")");
    LOG("============================================");
    LOG("Free heap at boot: %u bytes", ESP.getFreeHeap());

    // ── PSRAM check ───────────────────────────────────────────────────────────
    if (psramFound()) {
        LOG("PSRAM OK: %u bytes free", ESP.getFreePsram());
    } else {
        LOG("WARNING: No PSRAM — frame buffers will use SRAM.");
        LOG("  Recommend ESP32-S3-N8R8 (8 MB Flash, 8 MB PSRAM).");
    }

    // ── Display ───────────────────────────────────────────────────────────────
    if (!Display().begin()) {
        LOG("FATAL: HUB75 panel init failed.  Check wiring.  Halting.");
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    showSplash();

    // ── BLE ───────────────────────────────────────────────────────────────────
    Ble().begin();

    // ── Display FreeRTOS task ─────────────────────────────────────────────────
    Display().startTask();

    LOG("Boot complete.  Free heap: %u bytes", ESP.getFreeHeap());
    LOG("Advertising as '%s' — waiting for phone connection...", BLE_DEVICE_NAME);
}

// ── Splash: simple frame to confirm the panel is alive ────────────────────────

static void showSplash() {
    MatrixPanel_I2S_DMA* p = Display().panel();
    if (!p) return;

    p->clearScreen();

    // Cyan border
    uint16_t cyan  = p->color565(0, 200, 255);
    uint16_t white = p->color565(255, 255, 255);

    for (int x = 0; x < MATRIX_COLS; x++) {
        p->drawPixelRGB565(x, 0,               cyan);
        p->drawPixelRGB565(x, MATRIX_ROWS - 1, cyan);
    }
    for (int y = 1; y < MATRIX_ROWS - 1; y++) {
        p->drawPixelRGB565(0,              y, cyan);
        p->drawPixelRGB565(MATRIX_COLS-1, y, cyan);
    }

    // 2×2 white dot at centre to confirm pixel order
    int cx = MATRIX_COLS / 2 - 1;
    int cy = MATRIX_ROWS / 2 - 1;
    p->drawPixelRGB565(cx,   cy,   white);
    p->drawPixelRGB565(cx+1, cy,   white);
    p->drawPixelRGB565(cx,   cy+1, white);
    p->drawPixelRGB565(cx+1, cy+1, white);

    p->flipDMABuffer();
    delay(1200);
    p->clearScreen();
    p->flipDMABuffer();
}

// ── loop() ────────────────────────────────────────────────────────────────────

void loop() {
#if FRAMEON_DEBUG
    static uint32_t lastLog = 0;
    if (millis() - lastLog >= 15000) {
        lastLog = millis();
        LOG("Heap: %u | PSRAM: %u | BLE: %s",
            ESP.getFreeHeap(),
            psramFound() ? ESP.getFreePsram() : 0u,
            Ble().isConnected() ? "connected" : "advertising");
    }
#endif
    vTaskDelay(pdMS_TO_TICKS(1000));
}