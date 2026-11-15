#include "keypad_matrix.h"

namespace keypad {

uint16_t maskFromMatrix(const uint8_t columnBits[4]) {
    uint16_t mask = 0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            if (columnBits[r] & (1u << c)) {
                mask |= static_cast<uint16_t>(1u << kKeyAt[r][c]);
            }
        }
    }
    return mask;
}

}  // namespace keypad
