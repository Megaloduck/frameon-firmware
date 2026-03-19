#include "matrix.h"

MatrixPanel_I2S_DMA *matrix = nullptr;

void matrix_init() {
    // PANEL_HEIGHT=64 (virtual) forces the library to enable the E-pin and
    // generate 1/32 scan signals required by P4-2121-64x32-32S-JHT3.0.
    // Physical pixels only exist in rows 0–(REAL_HEIGHT-1).
    HUB75_I2S_CFG cfg(
        PANEL_WIDTH,   // 64
        PANEL_HEIGHT,  // 64 (virtual — do NOT use REAL_HEIGHT here)
        PANEL_CHAIN    // 1
    );

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
    cfg.gpio.e   = PIN_E;   // required for 1/32 scan
    cfg.gpio.lat = PIN_LAT;
    cfg.gpio.oe  = PIN_OE;
    cfg.gpio.clk = PIN_CLK;

    cfg.i2sspeed    = HUB75_I2S_CFG::HZ_10M;
    cfg.double_buff = true;
    cfg.driver      = HUB75_I2S_CFG::SHIFTREG;

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
