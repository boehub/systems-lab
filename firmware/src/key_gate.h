#pragma once

#include <cstdint>

// After a program starts, keys that were already held (the one that picked it
// from the menu) stay hidden from it until they have been let go once, so a
// program waiting for a key doesn't see a press nobody made for it.
class KeyGate {
public:
    void begin(uint16_t heldNow) { hidden_ = heldNow; }

    uint16_t filter(uint16_t keys) {
        hidden_ &= keys;  // a key that is up is no longer hidden
        return keys & static_cast<uint16_t>(~hidden_);
    }

private:
    uint16_t hidden_ = 0;
};
