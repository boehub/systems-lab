// Portable run loop: emulator core + platform interface + embedded ROM.
// Nothing in here knows whether it's running on a desktop or a microcontroller.

#include "chip8.h"
#include "platform.h"
#include "rom.h"  // generated from assembly by compiler/chip8asm.py

namespace {

constexpr uint32_t kFrameMs = 1000 / 60;  // timers tick at 60 Hz

// Instructions per 60 Hz frame; 12 is about 720 per second. A board build can
// change it with -DCHIP8_CYCLES_PER_FRAME=N.
#ifndef CHIP8_CYCLES_PER_FRAME
#define CHIP8_CYCLES_PER_FRAME 12
#endif
constexpr int kCyclesPerFrame = CHIP8_CYCLES_PER_FRAME;

}  // namespace

int main() {
    platform::init();

    // Static, not on the stack: the machine is about 6 KB and a microcontroller's
    // stack is typically 2 KB or less (the Pico's default).
    static Chip8 chip8;
    if (!chip8.loadRom(program, program_size)) {
        return 1;  // ROM doesn't fit in CHIP-8 memory
    }

    bool halted = false;  // set when the core hits an opcode it can't run

    while (platform::keepRunning()) {
        const uint32_t frameStart = platform::millisNow();

        const uint16_t keys = platform::readKeys();
        for (int k = 0; k < 16; ++k) {
            chip8.keypad[k] = (keys >> k) & 1;
        }

        if (!halted) {
            for (int c = 0; c < kCyclesPerFrame; ++c) {
                if (!chip8.cycle()) {
                    halted = true;
                    break;
                }
            }
            chip8.tickTimers();
        }

        platform::setBuzzer(chip8.soundTimer > 0);
        platform::drawFrame(chip8.display.data(), Chip8::kDisplayWidth,
                            Chip8::kDisplayHeight);

        const uint32_t elapsed = platform::millisNow() - frameStart;
        if (elapsed < kFrameMs) {
            platform::sleepMs(kFrameMs - elapsed);
        }
    }
    return 0;
}
