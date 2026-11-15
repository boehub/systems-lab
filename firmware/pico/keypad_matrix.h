#pragma once

// A 4x4 button matrix as the CHIP-8 keypad. No Pico SDK in here, so it is
// unit-tested on the desktop (tests/test_pico_logic.cpp).

#include <cstdint>

namespace keypad {

// Which CHIP-8 key sits at each [row][column]. This is the standard CHIP-8
// layout, which is also how a common 4x4 membrane keypad is usually printed
// (with A, B, C, D relabelled as needed).
constexpr uint8_t kKeyAt[4][4] = {
    {0x1, 0x2, 0x3, 0xC},
    {0x4, 0x5, 0x6, 0xD},
    {0x7, 0x8, 0x9, 0xE},
    {0xA, 0x0, 0xB, 0xF},
};

// columnBits[r] has bit c set when the button at row r, column c was seen
// pressed while row r was being driven. Returns a CHIP-8 key mask (bit k set
// = key k held).
uint16_t maskFromMatrix(const uint8_t columnBits[4]);

// Contact bounce: a switch can flicker for a few milliseconds. A key counts as
// pressed once two scans in a row agree it is down, and as released once two
// in a row agree it is up. (At one scan per 60 Hz frame that adds under
// 17 ms of delay.)
class Debouncer {
public:
    uint16_t update(uint16_t raw) {
        stable_ = (raw & previous_) | (stable_ & (raw | previous_));
        previous_ = raw;
        return stable_;
    }

private:
    uint16_t previous_ = 0;
    uint16_t stable_ = 0;
};

}  // namespace keypad
