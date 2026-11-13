#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// CHIP-8 machine state. The core does no I/O (no files, no stdio) so the same
// code runs on a desktop and on a microcontroller (see ../../firmware).
class Chip8 {
public:
    static constexpr int kMemorySize = 4096;
    static constexpr int kStartAddress = 0x200;
    static constexpr int kDisplayWidth = 64;
    static constexpr int kDisplayHeight = 32;

    // The built-in font (16 hex digits, 5 bytes each) lives here, below the
    // program area. The FX29 opcode points I at a digit's glyph.
    static constexpr int kFontAddress = 0x50;
    static constexpr int kFontGlyphBytes = 5;

    // A few opcodes behave differently on different historical interpreters.
    // The defaults match what most ROMs written since the 1990s expect.
    struct Quirks {
        // 8XY6 / 8XYE: shift VY into VX (original COSMAC VIP), instead of
        // shifting VX in place.
        bool shiftUsesVy = false;
        // BNNN: jump to NNN + VX (CHIP-48 / SUPER-CHIP), instead of NNN + V0.
        bool jumpUsesVx = false;
        // FX55 / FX65: leave I advanced by X+1 afterwards (original COSMAC
        // VIP), instead of unchanged.
        bool loadStoreIncrementsI = false;
    };

    Chip8();

    // Copy a ROM image into memory at 0x200. Returns false if it doesn't fit.
    bool loadRom(const uint8_t* data, size_t size);

    // Run one fetch / decode / execute step.
    // Returns false if the machine can't continue (unimplemented opcode,
    // stack overflow or underflow, a memory access out of range, or program
    // counter out of range); lastOpcode and lastPc say where.
    bool cycle();

    // Call at 60 Hz: counts the delay and sound timers down.
    void tickTimers();

    // Seed the random number generator behind the CXNN opcode. Seeding with
    // the same value gives the same sequence, which keeps tests repeatable.
    // On hardware, seed it from something unpredictable (a timer, a floating
    // ADC pin).
    void seedRandom(uint32_t seed);

    std::array<uint8_t, kMemorySize> memory{};
    std::array<uint8_t, 16> V{};  // general registers V0..VF (VF = flag)
    uint16_t I = 0;               // index register
    uint16_t pc = kStartAddress;  // program counter
    std::array<uint16_t, 16> stack{};
    uint8_t sp = 0;               // stack pointer
    uint8_t delayTimer = 0;
    uint8_t soundTimer = 0;
    std::array<uint8_t, kDisplayWidth * kDisplayHeight> display{};
    std::array<uint8_t, 16> keypad{};  // nonzero = key held down

    Quirks quirks;

    uint16_t lastOpcode = 0;  // most recently fetched opcode
    uint16_t lastPc = 0;      // address that opcode was fetched from

private:
    uint8_t nextRandom();
    uint32_t rngState_ = 0x2545F491;
};
