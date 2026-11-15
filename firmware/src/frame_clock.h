#pragma once

// Holds the main loop to 60 frames per second.
//
// A frame is 16.67 ms, which a millisecond clock can't count directly. Waiting
// a flat 16 ms per frame runs 4% fast (62.5 Hz), which makes the CHIP-8 timers
// and everything paced by them too fast. This keeps the remainder: frames take
// 16, 17, 17 ms in turn, exactly 1000 ms per 60 frames. All arithmetic is
// modular, so the 32-bit millisecond clock wrapping around (after 49 days)
// doesn't matter.

#include <cstdint>

class FrameClock {
public:
    explicit FrameClock(uint32_t nowMs) : due_(nowMs) {}

    // Call once per frame, after the frame's work. Returns how many ms to
    // sleep so the next frame starts on time (0 if already late). If the loop
    // has fallen more than 100 ms behind (a slow redraw, a pause), it starts
    // over from now instead of rushing to catch up.
    uint32_t msUntilNextFrame(uint32_t nowMs) {
        remainder_ += 50;               // 50/3 ms = 16.67 ms
        due_ += remainder_ / 3;
        remainder_ %= 3;
        const int32_t wait = static_cast<int32_t>(due_ - nowMs);
        if (wait > 0) {
            return static_cast<uint32_t>(wait);
        }
        if (wait < -100) {
            due_ = nowMs;
            remainder_ = 0;
        }
        return 0;
    }

private:
    uint32_t due_;
    uint32_t remainder_ = 0;
};
