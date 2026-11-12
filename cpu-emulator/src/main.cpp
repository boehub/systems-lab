#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

#include "chip8.h"

// Print the 64x32 display to the terminal. Lives here (not in Chip8) so the
// emulator core stays free of I/O and can be ported to a microcontroller.
static void printDisplay(const Chip8& chip8) {
    for (int row = 0; row < Chip8::kDisplayHeight; ++row) {
        for (int col = 0; col < Chip8::kDisplayWidth; ++col) {
            std::putchar(chip8.display[row * Chip8::kDisplayWidth + col] ? '#'
                                                                         : '.');
        }
        std::putchar('\n');
    }
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <rom>\n", argv[0]);
        return 1;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "could not open ROM: %s\n", argv[1]);
        return 1;
    }
    const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());

    Chip8 chip8;
    if (!chip8.loadRom(rom.data(), rom.size())) {
        std::fprintf(stderr, "ROM too large: %s\n", argv[1]);
        return 1;
    }

    // Placeholder run loop: step until we hit an opcode we don't support yet.
    // Real-time timing and input come later.
    int cycles = 0;
    while (cycles < 1000) {
        if (!chip8.cycle()) {
            std::fprintf(stderr,
                         "Can't execute opcode 0x%04X at 0x%03X (not "
                         "implemented, or a stack error)\n",
                         chip8.lastOpcode, chip8.lastPc);
            break;
        }
        ++cycles;
    }
    printDisplay(chip8);
    std::printf("ran %d cycles, pc = 0x%03X\n", cycles, chip8.pc);
    return 0;
}
