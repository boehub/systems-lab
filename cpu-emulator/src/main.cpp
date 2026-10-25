#include <cstdio>

#include "chip8.h"

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
    std::printf("ran %d cycles, pc = 0x%03X\n", cycles, chip8.pc);
    return 0;
}
