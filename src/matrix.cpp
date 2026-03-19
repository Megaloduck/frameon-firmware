#include "matrix.h"

MatrixPanel_I2S_DMA *matrix = nullptr;

void matrix_init() {
    HUB75_I2S_CFG cfg(
        PANEL_WIDTH,
        PANEL_HEIGHT,
        PANEL_CHAIN
    );

    // Custom pin mapping
    cfg.gpio.r1  = PIN_R1;
    cfg.gpio.g1  = PIN_G1;
    cfg.gpio.b1  = PIN_B1;
    cfg.gpio.r2  = PIN_R2;
    cfg.gpio.g2  = PIN_G2;
    cfg.gpio.b2  = PIN_B2;
    cfg.gpio.a   = PIN_A;
    cfg.gpio.b   = PIN_B;
    cfg.gpio.c   = PIN_C;
    cfg.gpio.d   = PIN_D;
    cfg.gpio.e   = PIN_E;
    cfg.gpio.lat = PIN_LAT;
    cfg.gpio.oe  = PIN_OE;
    cfg.gpio.clk = PIN_CLK;

    // ESP32-S3 DMA settings
    cfg.i2sspeed = HUB75_I2S_CFG::HZ_10M;
    cfg.double_buff = true;    // smooth updates, no flicker
    cfg.driver = HUB75_I2S_CFG::SHIFTREG;  // standard HUB75

    matrix = new MatrixPanel_I2S_DMA(cfg);
    matrix->begin();
    matrix->setBrightness8(PANEL_BRIGHTNESS);
    matrix->clearScreen();
    matrix->flipDMABuffer();
}

void matrix_brightness(uint8_t value) {
    if (matrix) matrix->setBrightness8(value);
}

void matrix_clear() {
    if (matrix) {
        matrix->clearScreen();
        matrix->flipDMABuffer();
    }
}
