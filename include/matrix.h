#pragma once
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "config.h"

extern MatrixPanel_I2S_DMA *matrix;

void matrix_init();
void matrix_brightness(uint8_t value);
void matrix_clear();

// Colour helpers (RGB888 → RGB565 inline)
inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return matrix->color565(r, g, b);
}

// Common palette
#define COL_BLACK   rgb(0,   0,   0)
#define COL_WHITE   rgb(255, 255, 255)
#define COL_GREEN   rgb(0,   229, 160)   // accent
#define COL_BLUE    rgb(74,  158, 255)   // clock
#define COL_RED     rgb(255, 74,  74)
#define COL_ORANGE  rgb(255, 179, 71)    // pomodoro
#define COL_SPOTIFY rgb(29,  185, 84)
#define COL_DIM     rgb(20,  20,  20)
