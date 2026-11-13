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

static void testFontLoaded() {
    Chip8 c;
    CHECK(c.memory[Chip8::kFontAddress] == 0xF0);  // first row of '0'
    CHECK(c.memory[Chip8::kFontAddress + 5] == 0x20);  // first row of '1'
    CHECK(c.memory[Chip8::kFontAddress + 15 * 5 + 4] == 0x80);  // last row of 'F'
}

static void testRegisterCopyAndLogic() {
    Chip8 c = machine({0x8210});  // 8XY0: V2 = V1
    c.V[1] = 0x5A;
    CHECK(c.cycle());
    CHECK(c.V[2] == 0x5A);

    Chip8 o = machine({0x8121});  // 8XY1: V1 |= V2
    o.V[1] = 0xF0;
    o.V[2] = 0x0F;
    CHECK(o.cycle());
    CHECK(o.V[1] == 0xFF);

    Chip8 a = machine({0x8122});  // 8XY2: V1 &= V2
    a.V[1] = 0xFC;
    a.V[2] = 0x3F;
    CHECK(a.cycle());
    CHECK(a.V[1] == 0x3C);

    Chip8 x = machine({0x8123});  // 8XY3: V1 ^= V2
    x.V[1] = 0xFF;
    x.V[2] = 0x0F;
    CHECK(x.cycle());
    CHECK(x.V[1] == 0xF0);
}

static void testAddWithCarry() {
    Chip8 c = machine({0x8124});  // 8XY4: V1 += V2, VF = carry
    c.V[1] = 200;
    c.V[2] = 100;
    CHECK(c.cycle());
    CHECK(c.V[1] == 44);  // 300 - 256
    CHECK(c.V[0xF] == 1);

    Chip8 d = machine({0x8124});
    d.V[1] = 200;
    d.V[2] = 55;  // exactly 255: no carry
    d.V[0xF] = 1;  // a stale flag must be cleared
    CHECK(d.cycle());
    CHECK(d.V[1] == 255);
    CHECK(d.V[0xF] == 0);

    Chip8 e = machine({0x8124});
    e.V[1] = 255;
    e.V[2] = 1;
    CHECK(e.cycle());
    CHECK(e.V[1] == 0);
    CHECK(e.V[0xF] == 1);
}

static void testSubtractAndReverseSubtract() {
    Chip8 a = machine({0x8125});  // 8XY5: V1 -= V2, VF = 1 if no borrow
    a.V[1] = 10;
    a.V[2] = 3;
    CHECK(a.cycle());
    CHECK(a.V[1] == 7 && a.V[0xF] == 1);

    Chip8 b = machine({0x8125});
    b.V[1] = 3;
    b.V[2] = 10;
    CHECK(b.cycle());
    CHECK(b.V[1] == 249 && b.V[0xF] == 0);  // wrapped, borrow

    Chip8 c = machine({0x8125});
    c.V[1] = 5;
    c.V[2] = 5;
    CHECK(c.cycle());
    CHECK(c.V[1] == 0 && c.V[0xF] == 1);  // equal: no borrow

    Chip8 d = machine({0x8127});  // 8XY7: V1 = V2 - V1, VF = 1 if no borrow
    d.V[1] = 3;
    d.V[2] = 10;
    CHECK(d.cycle());
    CHECK(d.V[1] == 7 && d.V[0xF] == 1);

    Chip8 e = machine({0x8127});
    e.V[1] = 10;
    e.V[2] = 3;
    CHECK(e.cycle());
    CHECK(e.V[1] == 249 && e.V[0xF] == 0);
}

static void testFlagWinsWhenTargetIsVF() {
    Chip8 c = machine({0x8F14});  // VF += V1: result is lost, carry flag stays
    c.V[0xF] = 200;
    c.V[1] = 100;
    CHECK(c.cycle());
    CHECK(c.V[0xF] == 1);

    Chip8 d = machine({0x8F15});  // VF -= V1 with a borrow: flag is 0
    d.V[0xF] = 5;
    d.V[1] = 10;
    CHECK(d.cycle());
    CHECK(d.V[0xF] == 0);
}

static void testShifts() {
    Chip8 r = machine({0x8126});  // 8XY6: shift VX right in place
    r.V[1] = 0b101;
    r.V[2] = 0xFF;  // must be ignored by default
    CHECK(r.cycle());
    CHECK(r.V[1] == 0b10 && r.V[0xF] == 1);

    Chip8 l = machine({0x812E});  // 8XYE: shift VX left in place
    l.V[1] = 0x81;
    l.V[2] = 0x00;
    CHECK(l.cycle());
    CHECK(l.V[1] == 0x02 && l.V[0xF] == 1);

    Chip8 l2 = machine({0x812E});
    l2.V[1] = 0x40;
    CHECK(l2.cycle());
    CHECK(l2.V[1] == 0x80 && l2.V[0xF] == 0);

    Chip8 rq = machine({0x8126});  // quirk: shift VY into VX
    rq.quirks.shiftUsesVy = true;
    rq.V[1] = 0;
    rq.V[2] = 0b11;
    CHECK(rq.cycle());
    CHECK(rq.V[1] == 1 && rq.V[0xF] == 1);

    Chip8 lq = machine({0x812E});
    lq.quirks.shiftUsesVy = true;
    lq.V[2] = 0x81;
    CHECK(lq.cycle());
    CHECK(lq.V[1] == 0x02 && lq.V[0xF] == 1);
}

static void testBadArithmeticLowNibbles() {
    for (uint16_t op : {0x8128, 0x8129, 0x812A, 0x812B, 0x812C, 0x812D, 0x812F}) {
        Chip8 c = machine({op});
        CHECK(!c.cycle());
    }
}

static void testJumpWithOffset() {
    Chip8 c = machine({0xB300});  // BNNN: pc = NNN + V0
    c.V[0] = 4;
    c.V[3] = 0x77;  // ignored by default
    CHECK(c.cycle());
    CHECK(c.pc == 0x304);

    Chip8 q = machine({0xB300});  // quirk: pc = NNN + VX (X = 3 here)
    q.quirks.jumpUsesVx = true;
    q.V[0] = 4;
    q.V[3] = 0x10;
    CHECK(q.cycle());
    CHECK(q.pc == 0x310);

    Chip8 o = machine({0xBFFF});  // lands past the end of memory
    o.V[0] = 0xFF;
    CHECK(o.cycle());
    CHECK(!o.cycle());  // fetching from there is refused
}

static void testRandom() {
    // Same seed, same sequence. Different seed, different sequence.
    auto draw = [](uint32_t seed, uint8_t mask) {
        Chip8 c = machine({static_cast<uint16_t>(0xC100 | mask)});
        c.seedRandom(seed);
        std::array<uint8_t, 32> out{};
        for (auto& v : out) {
            c.pc = 0x200;
            c.cycle();
            v = c.V[1];
        }
        return out;
    };
    CHECK(draw(1234, 0xFF) == draw(1234, 0xFF));
    CHECK(draw(1234, 0xFF) != draw(5678, 0xFF));

    // The mask limits the result.
    bool allSmall = true;
    for (uint8_t v : draw(99, 0x0F)) allSmall = allSmall && v < 16;
    CHECK(allSmall);
    bool allZero = true;
    for (uint8_t v : draw(99, 0x00)) allZero = allZero && v == 0;
    CHECK(allZero);

    // It isn't stuck on one value.
    const auto many = draw(42, 0xFF);
    bool varied = false;
    for (uint8_t v : many) varied = varied || v != many[0];
    CHECK(varied);

    // Seed 0 is replaced (xorshift can't run from zero).
    CHECK(draw(0, 0xFF) == draw(1, 0xFF));
}

static void testKeySkips() {
    Chip8 c = machine({0xE19E});  // EX9E: skip if key V1 is held
    c.V[1] = 0xA;
    c.keypad[0xA] = 1;
    CHECK(c.cycle());
    CHECK(c.pc == 0x204);

    Chip8 d = machine({0xE19E});
    d.V[1] = 0xA;
    CHECK(d.cycle());
    CHECK(d.pc == 0x202);

    Chip8 e = machine({0xE1A1});  // EXA1: skip if key V1 is NOT held
    e.V[1] = 0xA;
    CHECK(e.cycle());
    CHECK(e.pc == 0x204);

    Chip8 f = machine({0xE1A1});
    f.V[1] = 0xA;
    f.keypad[0xA] = 1;
    CHECK(f.cycle());
    CHECK(f.pc == 0x202);

    // Only the low nibble picks the key, so 0x1A means key A and can't
    // index past the keypad.
    Chip8 g = machine({0xE19E});
    g.V[1] = 0x1A;
    g.keypad[0xA] = 1;
    CHECK(g.cycle());
    CHECK(g.pc == 0x204);

    Chip8 h = machine({0xE1FF});  // not a real instruction
    CHECK(!h.cycle());
}

static void testWaitForKey() {
    Chip8 c = machine({0xF30A});  // FX0A: wait for a key
    CHECK(c.cycle());
    CHECK(c.pc == 0x200);  // no key: the same instruction runs again
    CHECK(c.V[3] == 0);

    c.keypad[7] = 1;
    CHECK(c.cycle());
    CHECK(c.V[3] == 7);
    CHECK(c.pc == 0x202);  // moved on

    Chip8 d = machine({0xF30A});
    d.keypad[9] = 1;
    d.keypad[3] = 1;
    CHECK(d.cycle());
    CHECK(d.V[3] == 3);  // lowest held key
}

static void testTimerOpcodes() {
    Chip8 c = machine({0xF215, 0xF418, 0xF507});
    c.V[2] = 30;
    c.V[4] = 12;
    CHECK(c.cycle());  // FX15: delay = V2
    CHECK(c.delayTimer == 30);
    CHECK(c.cycle());  // FX18: sound = V4
    CHECK(c.soundTimer == 12);
    c.delayTimer = 9;
    CHECK(c.cycle());  // FX07: V5 = delay
    CHECK(c.V[5] == 9);
}

static void testAddToIndex() {
    Chip8 c = machine({0xF11E});  // FX1E: I += V1
    c.I = 0x300;
    c.V[1] = 0x20;
    c.V[0xF] = 7;
    CHECK(c.cycle());
    CHECK(c.I == 0x320);
    CHECK(c.V[0xF] == 7);  // flag untouched
}

static void testFontOpcode() {
    Chip8 c = machine({0xF129});  // FX29: I = glyph for digit V1
    c.V[1] = 0xA;
    CHECK(c.cycle());
    CHECK(c.I == Chip8::kFontAddress + 0xA * Chip8::kFontGlyphBytes);

    Chip8 d = machine({0xF129});  // only the low nibble counts
    d.V[1] = 0x1A;
    CHECK(d.cycle());
    CHECK(d.I == Chip8::kFontAddress + 0xA * Chip8::kFontGlyphBytes);

    // Point I at '0' and draw it at (0,0).
    Chip8 e = machine({0xF129, 0xD235});
    e.V[1] = 0;
    CHECK(e.cycle());
    CHECK(e.cycle());
    CHECK(e.display[0] == 1 && e.display[3] == 1);  // ####
    CHECK(e.display[64 + 1] == 0);                  // #..#
}

static void testBcd() {
    Chip8 c = machine({0xF133});  // FX33: decimal digits of V1 at I..I+2
    c.V[1] = 156;
    c.I = 0x300;
    CHECK(c.cycle());
    CHECK(c.memory[0x300] == 1 && c.memory[0x301] == 5 && c.memory[0x302] == 6);

    Chip8 d = machine({0xF133});
    d.V[1] = 7;
    d.I = 0x300;
    CHECK(d.cycle());
    CHECK(d.memory[0x300] == 0 && d.memory[0x301] == 0 && d.memory[0x302] == 7);

    Chip8 e = machine({0xF133});
    e.V[1] = 255;
    e.I = 0x300;
    CHECK(e.cycle());
    CHECK(e.memory[0x300] == 2 && e.memory[0x301] == 5 && e.memory[0x302] == 5);

    Chip8 last = machine({0xF133});  // last three bytes of RAM: fits
    last.V[1] = 123;
    last.I = 0xFFD;
    CHECK(last.cycle());
    CHECK(last.memory[0xFFF] == 3);

    Chip8 over = machine({0xF133});  // one byte too far: refused
    over.I = 0xFFE;
    CHECK(!over.cycle());
}

static void testStoreAndLoadRegisters() {
    Chip8 s = machine({0xF355});  // FX55: store V0..V3 at I
    s.V[0] = 1;
    s.V[1] = 2;
    s.V[2] = 3;
    s.V[3] = 4;
    s.V[4] = 99;
    s.I = 0x300;
    CHECK(s.cycle());
    CHECK(s.memory[0x300] == 1 && s.memory[0x303] == 4);
    CHECK(s.memory[0x304] == 0);  // V4 was not stored
    CHECK(s.I == 0x300);          // I unchanged by default

    Chip8 q = machine({0xF355});  // quirk: I advances by X+1
    q.quirks.loadStoreIncrementsI = true;
    q.I = 0x300;
    CHECK(q.cycle());
    CHECK(q.I == 0x304);

    Chip8 l = machine({0xF265});  // FX65: load V0..V2 from I
    l.memory[0x300] = 9;
    l.memory[0x301] = 8;
    l.memory[0x302] = 7;
    l.memory[0x303] = 6;
    l.V[3] = 55;
    l.I = 0x300;
    CHECK(l.cycle());
    CHECK(l.V[0] == 9 && l.V[1] == 8 && l.V[2] == 7);
    CHECK(l.V[3] == 55);  // V3 was not loaded

    Chip8 lq = machine({0xF265});
    lq.quirks.loadStoreIncrementsI = true;
    lq.I = 0x300;
    CHECK(lq.cycle());
    CHECK(lq.I == 0x303);

    Chip8 edge = machine({0xF055});  // X = 0 at the last byte: fits
    edge.V[0] = 5;
    edge.I = 0xFFF;
    CHECK(edge.cycle());
    CHECK(edge.memory[0xFFF] == 5);

    Chip8 over = machine({0xF155});  // X = 1 at the last byte: refused
    over.I = 0xFFF;
    CHECK(!over.cycle());

    Chip8 overLoad = machine({0xF165});
    overLoad.I = 0xFFF;
    CHECK(!overLoad.cycle());
}

static void testUnknownEAndFOpcodes() {
    for (uint16_t op : {0xF1FF, 0xF100, 0xF1AA, 0xE1FF, 0xE100, 0x0123}) {
        Chip8 c = machine({op});
        CHECK(!c.cycle());
    }
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
    testFontLoaded();
    testRegisterCopyAndLogic();
    testAddWithCarry();
    testSubtractAndReverseSubtract();
    testFlagWinsWhenTargetIsVF();
    testShifts();
    testBadArithmeticLowNibbles();
    testJumpWithOffset();
    testRandom();
    testKeySkips();
    testWaitForKey();
    testTimerOpcodes();
    testAddToIndex();
    testFontOpcode();
    testBcd();
    testStoreAndLoadRegisters();
    testUnknownEAndFOpcodes();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
