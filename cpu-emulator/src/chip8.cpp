#include "chip8.h"

Chip8::Chip8() = default;

bool Chip8::loadRom(const uint8_t* data, size_t size) {
    if (size > memory.size() - kStartAddress) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        memory[kStartAddress + i] = data[i];
    }
    return true;
}

void Chip8::tickTimers() {
    if (delayTimer > 0) --delayTimer;
    if (soundTimer > 0) --soundTimer;
}

bool Chip8::cycle() {
    // An opcode is 2 bytes, so the program counter must leave room for both.
    if (pc > kMemorySize - 2) {
        return false;
    }

    // Fetch: big-endian.
    lastPc = pc;
    const uint16_t opcode = (memory[pc] << 8) | memory[pc + 1];
    lastOpcode = opcode;
    pc += 2;

    // Decode the common fields.
    const uint8_t x = (opcode & 0x0F00) >> 8;
    const uint8_t y = (opcode & 0x00F0) >> 4;
    const uint8_t n = opcode & 0x000F;
    const uint8_t nn = opcode & 0x00FF;
    const uint16_t nnn = opcode & 0x0FFF;

    // Execute. Only some opcodes so far; the rest come next.
    switch (opcode & 0xF000) {
        case 0x0000:
            if (opcode == 0x00E0) {  // 00E0: clear the screen
                display.fill(0);
                return true;
            }
            if (opcode == 0x00EE) {  // 00EE: return from subroutine
                if (sp == 0) return false;  // nothing to return to
                pc = stack[--sp];
                return true;
            }
            break;
        case 0x1000:  // 1NNN: jump to NNN
            pc = nnn;
            return true;
        case 0x2000:  // 2NNN: call subroutine at NNN
            if (sp >= stack.size()) return false;  // stack full (16 levels)
            stack[sp++] = pc;  // pc already points at the next instruction
            pc = nnn;
            return true;
        case 0x3000:  // 3XNN: skip next instruction if VX == NN
            if (V[x] == nn) pc += 2;
            return true;
        case 0x4000:  // 4XNN: skip next instruction if VX != NN
            if (V[x] != nn) pc += 2;
            return true;
        case 0x5000:  // 5XY0: skip next instruction if VX == VY
            if (n != 0) break;  // low nibble must be 0
            if (V[x] == V[y]) pc += 2;
            return true;
        case 0x6000:  // 6XNN: VX = NN
            V[x] = nn;
            return true;
        case 0x7000:  // 7XNN: VX += NN (no carry flag)
            V[x] += nn;
            return true;
        case 0x9000:  // 9XY0: skip next instruction if VX != VY
            if (n != 0) break;  // low nibble must be 0
            if (V[x] != V[y]) pc += 2;
            return true;
        case 0xA000:  // ANNN: I = NNN
            I = nnn;
            return true;
        case 0xD000: {  // DXYN: draw N-row sprite from memory[I] at (VX, VY)
            const uint8_t xPos = V[x] % kDisplayWidth;
            const uint8_t yPos = V[y] % kDisplayHeight;
            V[0xF] = 0;
            for (int row = 0; row < n; ++row) {
                if (yPos + row >= kDisplayHeight) break;  // clip at bottom
                if (I + row >= kMemorySize) break;        // don't read past RAM
                const uint8_t spriteByte = memory[I + row];
                for (int col = 0; col < 8; ++col) {
                    if (xPos + col >= kDisplayWidth) break;  // clip at right
                    if (spriteByte & (0x80 >> col)) {
                        uint8_t& pixel =
                            display[(yPos + row) * kDisplayWidth + (xPos + col)];
                        if (pixel) V[0xF] = 1;  // a lit pixel got turned off
                        pixel ^= 1;
                    }
                }
            }
            return true;
        }
        default:
            break;
    }

    return false;  // unimplemented opcode
}
