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

    Chip8();

    // Copy a ROM image into memory at 0x200. Returns false if it doesn't fit.
    bool loadRom(const uint8_t* data, size_t size);

    // Run one fetch / decode / execute step.
    // Returns false if the machine can't continue (unimplemented opcode,
    // stack overflow or underflow, or program counter out of range);
    // lastOpcode and lastPc say where.
    bool cycle();

    // Call at 60 Hz: counts the delay and sound timers down.
    void tickTimers();

    std::array<uint8_t, kMemorySize> memory{};
    std::array<uint8_t, 16> V{};  // general registers V0..VF (VF = flag)
    uint16_t I = 0;               // index register
    uint16_t pc = kStartAddress;  // program counter
    std::array<uint16_t, 16> stack{};
    uint8_t sp = 0;               // stack pointer
    uint8_t delayTimer = 0;
    uint8_t soundTimer = 0;
    std::array<uint8_t, kDisplayWidth * kDisplayHeight> display{};
    std::array<uint8_t, 16> keypad{};

    uint16_t lastOpcode = 0;  // most recently fetched opcode
    uint16_t lastPc = 0;      // address that opcode was fetched from
};
