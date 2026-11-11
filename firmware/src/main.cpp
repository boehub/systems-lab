// Portable run loop: emulator core + platform interface + embedded ROM.
// Nothing in here knows whether it's running on a desktop or a microcontroller.

#include "chip8.h"
#include "platform.h"
#include "rom.h"  // generated from assembly by compiler/chip8asm.py

namespace {

constexpr uint32_t kFrameMs = 1000 / 60;  // timers tick at 60 Hz
constexpr int kCyclesPerFrame = 10;       // about 600 instructions per second

}  // namespace

int main() {
    platform::init();

    Chip8 chip8;
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

        platform::drawFrame(chip8.display.data(), Chip8::kDisplayWidth,
                            Chip8::kDisplayHeight);

        const uint32_t elapsed = platform::millisNow() - frameStart;
        if (elapsed < kFrameMs) {
            platform::sleepMs(kFrameMs - elapsed);
        }
    }
    return 0;
}
