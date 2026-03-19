#pragma once
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "config.h"

extern MatrixPanel_I2S_DMA *matrix;

void matrix_init();
void matrix_brightness(uint8_t value);
void matrix_clear();

// Colour helper — safe to call any time after matrix_init()
inline uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    // FIX: direct RGB565 conversion instead of calling matrix->color565()
    // which would crash if matrix is null. Formula is identical.
    return ((uint16_t)(r & 0xF8) << 8)
         | ((uint16_t)(g & 0xFC) << 3)
         | ((uint16_t)(b >> 3));
}

// FIX: use inline functions instead of macros so they are evaluated
// at call time (not at compile/macro-expansion time), safe pre-init.
inline uint16_t COL_BLACK()   { return rgb(0,   0,   0);   }
inline uint16_t COL_WHITE()   { return rgb(255, 255, 255); }
inline uint16_t COL_GREEN()   { return rgb(0,   229, 160); }  // accent
inline uint16_t COL_BLUE()    { return rgb(74,  158, 255); }  // clock
inline uint16_t COL_RED()     { return rgb(255, 74,  74);  }
inline uint16_t COL_ORANGE()  { return rgb(255, 179, 71);  }  // pomodoro
inline uint16_t COL_SPOTIFY() { return rgb(29,  185, 84);  }
inline uint16_t COL_DIM()     { return rgb(20,  20,  20);  }
