#pragma once

// Drawing text into a one-byte-per-pixel image, for the program menu. A 5x7
// font: 6 pixels per character across, so 21 characters fit on a 128-wide
// display. No hardware in here; it is unit-tested on the desktop.

#include <cstdint>

namespace text {

constexpr int kGlyphWidth = 5;
constexpr int kGlyphHeight = 7;
constexpr int kAdvance = 6;  // pixels from one character to the next

// Width in pixels of `s` (at most `maxChars` characters; -1 = all).
int width(const char* s, int maxChars = -1);

// Draw `s` with its top-left at (x, y). Lit pixels are set to `value`; the
// background is left alone. Lower case is drawn as upper case, and characters
// the font doesn't have appear as '?'. Anything outside the image is clipped.
void draw(uint8_t* pixels, int w, int h, int x, int y, const char* s,
          uint8_t value = 1, int maxChars = -1);

// Fill a rectangle (clipped to the image).
void fillRect(uint8_t* pixels, int w, int h, int x, int y, int rw, int rh,
              uint8_t value);

}  // namespace text
