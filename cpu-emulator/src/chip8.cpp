#include "chip8.h"

namespace {

// The standard CHIP-8 hex font: digits 0-F, 4 pixels wide and 5 rows tall.
constexpr uint8_t kFont[16 * Chip8::kFontGlyphBytes] = {
    0xF0, 0x90, 0x90, 0x90, 0xF0,  // 0
    0x20, 0x60, 0x20, 0x20, 0x70,  // 1
    0xF0, 0x10, 0xF0, 0x80, 0xF0,  // 2
    0xF0, 0x10, 0xF0, 0x10, 0xF0,  // 3
    0x90, 0x90, 0xF0, 0x10, 0x10,  // 4
    0xF0, 0x80, 0xF0, 0x10, 0xF0,  // 5
    0xF0, 0x80, 0xF0, 0x90, 0xF0,  // 6
    0xF0, 0x10, 0x20, 0x40, 0x40,  // 7
    0xF0, 0x90, 0xF0, 0x90, 0xF0,  // 8
    0xF0, 0x90, 0xF0, 0x10, 0xF0,  // 9
    0xF0, 0x90, 0xF0, 0x90, 0x90,  // A
    0xE0, 0x90, 0xE0, 0x90, 0xE0,  // B
    0xF0, 0x80, 0x80, 0x80, 0xF0,  // C
    0xE0, 0x90, 0x90, 0x90, 0xE0,  // D
    0xF0, 0x80, 0xF0, 0x80, 0xF0,  // E
    0xF0, 0x80, 0xF0, 0x80, 0x80,  // F
};

static_assert(Chip8::kFontAddress + sizeof(kFont) <= Chip8::kStartAddress,
              "font must fit below the program area");

}  // namespace

Chip8::Chip8() {
    for (size_t i = 0; i < sizeof(kFont); ++i) {
        memory[kFontAddress + i] = kFont[i];
    }
}

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

void Chip8::seedRandom(uint32_t seed) {
    rngState_ = seed != 0 ? seed : 1;  // xorshift can't start from zero
}

// xorshift32: tiny, fast, and needs no library, so it works on any chip.
uint8_t Chip8::nextRandom() {
    uint32_t s = rngState_;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    rngState_ = s;
    return static_cast<uint8_t>(s >> 24);
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

    // Execute. Anything that falls out of the switch is unimplemented or
    // malformed, and stops the machine.
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
            // 0NNN (call native machine code) is deliberately not supported:
            // treating it as a no-op would make a program that runs off into
            // empty memory spin silently instead of stopping with an error.
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
        case 0x8000: {
            // In every case VF is written last, so if X is F the flag wins.
            const uint8_t vx = V[x];
            const uint8_t vy = V[y];
            switch (n) {
                case 0x0:  // 8XY0: VX = VY
                    V[x] = vy;
                    return true;
                case 0x1:  // 8XY1: VX |= VY
                    V[x] = vx | vy;
                    return true;
                case 0x2:  // 8XY2: VX &= VY
                    V[x] = vx & vy;
                    return true;
                case 0x3:  // 8XY3: VX ^= VY
                    V[x] = vx ^ vy;
                    return true;
                case 0x4: {  // 8XY4: VX += VY, VF = carry
                    const unsigned sum = vx + vy;
                    V[x] = static_cast<uint8_t>(sum);
                    V[0xF] = sum > 0xFF ? 1 : 0;
                    return true;
                }
                case 0x5:  // 8XY5: VX -= VY, VF = 1 if no borrow
                    V[x] = static_cast<uint8_t>(vx - vy);
                    V[0xF] = vx >= vy ? 1 : 0;
                    return true;
                case 0x6: {  // 8XY6: shift right, VF = bit shifted out
                    const uint8_t src = quirks.shiftUsesVy ? vy : vx;
                    V[x] = src >> 1;
                    V[0xF] = src & 1;
                    return true;
                }
                case 0x7:  // 8XY7: VX = VY - VX, VF = 1 if no borrow
                    V[x] = static_cast<uint8_t>(vy - vx);
                    V[0xF] = vy >= vx ? 1 : 0;
                    return true;
                case 0xE: {  // 8XYE: shift left, VF = bit shifted out
                    const uint8_t src = quirks.shiftUsesVy ? vy : vx;
                    V[x] = static_cast<uint8_t>(src << 1);
                    V[0xF] = (src >> 7) & 1;
                    return true;
                }
                default:
                    break;
            }
            break;
        }
        case 0x9000:  // 9XY0: skip next instruction if VX != VY
            if (n != 0) break;  // low nibble must be 0
            if (V[x] != V[y]) pc += 2;
            return true;
        case 0xA000:  // ANNN: I = NNN
            I = nnn;
            return true;
        case 0xB000:  // BNNN: jump to NNN + V0 (or VX with the quirk)
            pc = nnn + (quirks.jumpUsesVx ? V[x] : V[0]);
            return true;
        case 0xC000:  // CXNN: VX = random byte AND NN
            V[x] = nextRandom() & nn;
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
        case 0xE000:
            // Only the low 4 bits of VX pick the key, so a stray large value
            // can't index past the keypad.
            if (nn == 0x9E) {  // EX9E: skip if key VX is held
                if (keypad[V[x] & 0xF]) pc += 2;
                return true;
            }
            if (nn == 0xA1) {  // EXA1: skip if key VX is not held
                if (!keypad[V[x] & 0xF]) pc += 2;
                return true;
            }
            break;
        case 0xF000:
            switch (nn) {
                case 0x07:  // FX07: VX = delay timer
                    V[x] = delayTimer;
                    return true;
                case 0x0A:  // FX0A: wait for a key, store it in VX
                    for (uint8_t key = 0; key < 16; ++key) {
                        if (keypad[key]) {
                            V[x] = key;
                            return true;
                        }
                    }
                    pc -= 2;  // no key yet: run this instruction again
                    return true;
                case 0x15:  // FX15: delay timer = VX
                    delayTimer = V[x];
                    return true;
                case 0x18:  // FX18: sound timer = VX
                    soundTimer = V[x];
                    return true;
                case 0x1E:  // FX1E: I += VX
                    I += V[x];
                    return true;
                case 0x29:  // FX29: I = address of the font glyph for digit VX
                    I = kFontAddress + (V[x] & 0xF) * kFontGlyphBytes;
                    return true;
                case 0x33:  // FX33: store VX as 3 decimal digits at I, I+1, I+2
                    if (I + 2 >= kMemorySize) return false;
                    memory[I] = V[x] / 100;
                    memory[I + 1] = (V[x] / 10) % 10;
                    memory[I + 2] = V[x] % 10;
                    return true;
                case 0x55:  // FX55: store V0..VX into memory starting at I
                    if (I + x >= kMemorySize) return false;
                    for (int i = 0; i <= x; ++i) memory[I + i] = V[i];
                    if (quirks.loadStoreIncrementsI) I += x + 1;
                    return true;
                case 0x65:  // FX65: load V0..VX from memory starting at I
                    if (I + x >= kMemorySize) return false;
                    for (int i = 0; i <= x; ++i) V[i] = memory[I + i];
                    if (quirks.loadStoreIncrementsI) I += x + 1;
                    return true;
                default:
                    break;
            }
            break;
        default:
            break;
    }

    return false;  // unimplemented opcode
}
