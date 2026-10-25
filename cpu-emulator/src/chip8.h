#pragma once

#include <array>
#include <cstdint>

// CHIP-8 machine state. Kept free of OS-specific code so the core can be
// ported to a microcontroller later (see ../../firmware).
class Chip8 {
public:
    static constexpr int kMemorySize = 4096;
    static constexpr int kStartAddress = 0x200;
    static constexpr int kDisplayWidth = 64;
    static constexpr int kDisplayHeight = 32;

    Chip8();

    // Load a ROM image into memory at 0x200. Returns false on failure.
    bool loadRom(const char* path);

    // Run one fetch / decode / execute step.
    // Returns false if the opcode is not implemented yet.
    bool cycle();

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
};
