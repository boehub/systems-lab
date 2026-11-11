#include <cstdio>

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

    Chip8 chip8;
    if (!chip8.loadRom(argv[1])) {
        std::fprintf(stderr, "could not load ROM: %s\n", argv[1]);
        return 1;
    }

    // Placeholder run loop: step until we hit an opcode we don't support yet.
    // Display output, input, and timing come later.
    int cycles = 0;
    while (cycles < 1000 && chip8.cycle()) {
        ++cycles;
    }
    printDisplay(chip8);
    std::printf("ran %d cycles, pc = 0x%03X\n", cycles, chip8.pc);
    return 0;
}
