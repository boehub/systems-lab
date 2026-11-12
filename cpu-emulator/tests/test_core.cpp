// Tests for the emulator core. No framework: each CHECK prints a line on
// failure, and the exit code says whether everything passed.
//
//   make test

#include <cstdio>
#include <initializer_list>

#include "chip8.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        ++checks;                                                        \
        if (!(cond)) {                                                   \
            ++failures;                                                  \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond);          \
        }                                                                \
    } while (0)

// A machine with the given opcodes loaded at 0x200.
static Chip8 machine(std::initializer_list<uint16_t> opcodes) {
    Chip8 c;
    int addr = Chip8::kStartAddress;
    for (uint16_t op : opcodes) {
        c.memory[addr++] = op >> 8;
        c.memory[addr++] = op & 0xFF;
    }
    return c;
}

static void testJumpAndLoads() {
    Chip8 c = machine({0x6A2B, 0x7A01, 0xA123, 0x1300});
    CHECK(c.cycle());  // 6A2B: VA = 0x2B
    CHECK(c.V[0xA] == 0x2B);
    CHECK(c.cycle());  // 7A01: VA += 1
    CHECK(c.V[0xA] == 0x2C);
    CHECK(c.cycle());  // A123: I = 0x123
    CHECK(c.I == 0x123);
    CHECK(c.cycle());  // 1300: jump
    CHECK(c.pc == 0x300);
}

static void testCallAndReturn() {
    // 0x200: CALL 0x206   0x202: V0 = 7   0x204: JP 0x204   0x206: RET
    Chip8 c = machine({0x2206, 0x6007, 0x1204, 0x00EE});
    CHECK(c.cycle());  // CALL
    CHECK(c.pc == 0x206);
    CHECK(c.sp == 1);
    CHECK(c.stack[0] == 0x202);  // returns to the instruction after the call
    CHECK(c.cycle());  // RET
    CHECK(c.pc == 0x202);
    CHECK(c.sp == 0);
    CHECK(c.cycle());  // V0 = 7 runs after returning
    CHECK(c.V[0] == 7);
}

static void testNestedCalls() {
    // 0x200: CALL 0x204   0x202: JP 0x202   0x204: CALL 0x208
    // 0x206: RET          0x208: RET
    Chip8 c = machine({0x2204, 0x1202, 0x2208, 0x00EE, 0x00EE});
    CHECK(c.cycle());  // CALL 0x204
    CHECK(c.cycle());  // CALL 0x208
    CHECK(c.sp == 2);
    CHECK(c.cycle());  // RET -> 0x206
    CHECK(c.pc == 0x206);
    CHECK(c.cycle());  // RET -> 0x202
    CHECK(c.pc == 0x202);
    CHECK(c.sp == 0);
}

static void testReturnWithEmptyStack() {
    Chip8 c = machine({0x00EE});
    CHECK(!c.cycle());
    CHECK(c.lastOpcode == 0x00EE);
    CHECK(c.sp == 0);  // didn't underflow
}

static void testStackOverflow() {
    // CALL 0x200 calls itself forever; the stack holds 16 levels.
    Chip8 c = machine({0x2200});
    for (int i = 0; i < 16; ++i) {
        CHECK(c.cycle());
    }
    CHECK(c.sp == 16);
    CHECK(!c.cycle());  // the 17th call has nowhere to go
    CHECK(c.sp == 16);
}

static void testSkipEqualByte() {
    Chip8 c = machine({0x3A05});  // SE VA, 5
    c.V[0xA] = 5;
    CHECK(c.cycle());
    CHECK(c.pc == 0x204);  // skipped

    Chip8 d = machine({0x3A05});
    d.V[0xA] = 6;
    CHECK(d.cycle());
    CHECK(d.pc == 0x202);  // not skipped
}

static void testSkipNotEqualByte() {
    Chip8 c = machine({0x4A05});  // SNE VA, 5
    c.V[0xA] = 6;
    CHECK(c.cycle());
    CHECK(c.pc == 0x204);  // skipped

    Chip8 d = machine({0x4A05});
    d.V[0xA] = 5;
    CHECK(d.cycle());
    CHECK(d.pc == 0x202);  // not skipped
}

static void testSkipRegisters() {
    Chip8 c = machine({0x5120});  // SE V1, V2
    c.V[1] = 9;
    c.V[2] = 9;
    CHECK(c.cycle());
    CHECK(c.pc == 0x204);

    Chip8 d = machine({0x5120});
    d.V[1] = 9;
    d.V[2] = 8;
    CHECK(d.cycle());
    CHECK(d.pc == 0x202);

    Chip8 e = machine({0x9120});  // SNE V1, V2
    e.V[1] = 9;
    e.V[2] = 8;
    CHECK(e.cycle());
    CHECK(e.pc == 0x204);

    Chip8 f = machine({0x9120});
    f.V[1] = 9;
    f.V[2] = 9;
    CHECK(f.cycle());
    CHECK(f.pc == 0x202);
}

static void testSkipRegisterLowNibbleMustBeZero() {
    Chip8 c = machine({0x5121});  // not a real instruction
    CHECK(!c.cycle());
    Chip8 d = machine({0x9121});
    CHECK(!d.cycle());
}

static void testSkipThenExecute() {
    // SE V0, 0 skips the "V1 = 1" so V1 stays 0, then V2 = 2 runs.
    Chip8 c = machine({0x3000, 0x6101, 0x6202});
    CHECK(c.cycle());
    CHECK(c.cycle());
    CHECK(c.V[1] == 0);
    CHECK(c.V[2] == 2);
}

static void testDrawAndCollision() {
    // I -> glyph at 0x208, draw 5 rows at (0,0), draw again to erase.
    Chip8 c = machine({0xA208, 0xD015, 0xD015});
    const uint8_t glyph[] = {0xF0, 0x90, 0x90, 0x90, 0xF0};
    for (int i = 0; i < 5; ++i) c.memory[0x208 + i] = glyph[i];
    CHECK(c.cycle());
    CHECK(c.cycle());
    CHECK(c.display[0] == 1 && c.display[3] == 1);  // top row: ####
    CHECK(c.display[1 * 64 + 1] == 0);              // row 2: #..#
    CHECK(c.V[0xF] == 0);
    CHECK(c.cycle());  // drawing it again erases it
    CHECK(c.display[0] == 0);
    CHECK(c.V[0xF] == 1);
}

static void testDrawAtEndOfMemoryDoesNotOverrun() {
    Chip8 c = machine({0xAFFF, 0xD01F});  // I = 0xFFF, draw 15 rows
    CHECK(c.cycle());
    CHECK(c.cycle());  // must not read past the end of memory
}

static void testUnimplementedOpcodeStops() {
    Chip8 c = machine({0xFFFF});
    CHECK(!c.cycle());
    CHECK(c.lastOpcode == 0xFFFF);
    CHECK(c.lastPc == 0x200);
}

static void testProgramCounterOutOfRange() {
    Chip8 c;
    c.pc = 0xFFF;  // only one byte left, no room for an opcode
    CHECK(!c.cycle());
}

static void testRomTooLarge() {
    Chip8 c;
    static uint8_t big[Chip8::kMemorySize];
    CHECK(c.loadRom(big, Chip8::kMemorySize - Chip8::kStartAddress));
    CHECK(!c.loadRom(big, Chip8::kMemorySize - Chip8::kStartAddress + 1));
}

static void testTimers() {
    Chip8 c;
    c.delayTimer = 2;
    c.soundTimer = 1;
    c.tickTimers();
    CHECK(c.delayTimer == 1 && c.soundTimer == 0);
    c.tickTimers();
    CHECK(c.delayTimer == 0 && c.soundTimer == 0);  // stops at zero
}

int main() {
    testJumpAndLoads();
    testCallAndReturn();
    testNestedCalls();
    testReturnWithEmptyStack();
    testStackOverflow();
    testSkipEqualByte();
    testSkipNotEqualByte();
    testSkipRegisters();
    testSkipRegisterLowNibbleMustBeZero();
    testSkipThenExecute();
    testDrawAndCollision();
    testDrawAtEndOfMemoryDoesNotOverrun();
    testUnimplementedOpcodeStops();
    testProgramCounterOutOfRange();
    testRomTooLarge();
    testTimers();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
