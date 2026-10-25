#include "chip8.h"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

Chip8::Chip8() = default;

bool Chip8::loadRom(const char* path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
    if (rom.size() > memory.size() - kStartAddress) {
        return false;
    }
    for (size_t i = 0; i < rom.size(); ++i) {
        memory[kStartAddress + i] = rom[i];
    }
    return true;
}

bool Chip8::cycle() {
    // Fetch: opcodes are 2 bytes, big-endian.
    const uint16_t opcode = (memory[pc] << 8) | memory[pc + 1];
    pc += 2;

    // Decode the common fields.
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t nn = opcode & 0x00FF;
    const uint16_t nnn = opcode & 0x0FFF;

    // Execute. Only a few opcodes so far; the rest come next.
    switch (opcode & 0xF000) {
        case 0x0000:
            if (opcode == 0x00E0) {  // 00E0: clear the screen
                display.fill(0);
                return true;
            }
            break;
        case 0x1000:  // 1NNN: jump to NNN
            pc = nnn;
            return true;
        case 0x6000:  // 6XNN: VX = NN
            V[x] = nn;
            return true;
        case 0x7000:  // 7XNN: VX += NN (no carry flag)
            V[x] += nn;
            return true;
        case 0xA000:  // ANNN: I = NNN
            I = nnn;
            return true;
        default:
            break;
    }

    std::fprintf(stderr, "Unimplemented opcode 0x%04X at 0x%03X\n", opcode,
                 pc - 2);
    return false;
}
