#pragma once

// Turning the emulator's pixels into SSD1306 OLED memory. No Pico SDK in
// here, so it is unit-tested on the desktop (tests/test_pico_logic.cpp).

#include <cstdint>

namespace oled {

constexpr int kWidth = 128;
constexpr int kHeight = 64;
constexpr int kPages = kHeight / 8;               // the chip stores 8 rows per "page"
constexpr int kBufferSize = kWidth * kPages;      // 1024 bytes

// Scale a w x h image (one byte per pixel, 0 = off) up by the largest whole
// factor that fits, centre it, and pack it the way the SSD1306 wants it:
// out[page * 128 + x] has bit b set when the pixel at row page*8 + b is lit.
// A 64x32 CHIP-8 screen becomes 2x2 blocks filling the whole display.
void pack(const uint8_t* pixels, int w, int h, uint8_t out[kBufferSize]);

// Bit p of the result is set when page p differs between the two buffers.
// Only those pages need to be sent over I2C.
uint8_t changedPages(const uint8_t* a, const uint8_t* b);

}  // namespace oled
