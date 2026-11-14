#pragma once

// Desktop front-end logic that doesn't touch the operating system: command
// line options, the keyboard layout, key-hold timing, speed pacing, and
// screen rendering. Keeping it OS-free makes it unit-testable (see
// tests/test_frontend.cpp). The terminal itself lives in terminal.h.

#include <array>
#include <cstdint>
#include <string>

#include "chip8.h"

namespace frontend {

struct Options {
    std::string rom;
    int speed = 700;      // instructions per second
    int holdMs = 150;     // how long one key press counts as "held"
    bool headless = false;
    long cycles = 1000;   // headless only: how many instructions to run
    bool beep = false;    // ring the terminal bell while the sound timer runs
    uint32_t seed = 0;    // random seed (0 = pick one automatically)
    bool help = false;
    Chip8::Quirks quirks;
};

// Parse command-line arguments. Returns false and sets `error` on bad input.
bool parseArgs(int argc, const char* const* argv, Options& out,
               std::string& error);

// The text printed for --help (and after a usage error).
const char* usage();

// Map a typed character to a CHIP-8 key (0x0-0xF), or -1 if it isn't one.
// The keypad sits on the left of a QWERTY keyboard:
//     1 2 3 4        1 2 3 C
//     Q W E R   -->  4 5 6 D
//     A S D F        7 8 9 E
//     Z X C V        A 0 B F
int keyFromChar(char c);

// Terminals report key presses but never releases, so a key counts as held
// for a short window after its latest press (and keyboard auto-repeat keeps
// extending it while the key stays down).
class KeyHold {
public:
    explicit KeyHold(int holdMs) : holdMs_(holdMs) {}
    void press(int key, int64_t nowMs) { until_[key & 0xF] = nowMs + holdMs_; }
    bool isDown(int key, int64_t nowMs) const { return nowMs < until_[key & 0xF]; }

private:
    int holdMs_;
    std::array<int64_t, 16> until_{};
};

// Handle a chunk of typed bytes: register keypad presses, and return true if
// the user asked to quit (Esc on its own; arrow-key sequences are ignored).
bool handleInput(const std::string& bytes, KeyHold& keys, int64_t nowMs);

// Turns "instructions per second" into a whole number of instructions per
// 60 Hz frame, carrying the fraction forward so the average speed is exact.
class Pacer {
public:
    explicit Pacer(int speed) : speed_(speed) {}
    int nextFrame() {
        // Whole-number arithmetic, so 60 frames add up to exactly `speed`.
        acc_ += speed_;
        const int whole = acc_ / 60;
        acc_ %= 60;
        return whole;
    }

private:
    int speed_;
    int acc_ = 0;
};

// Run up to `cycles` instructions, then tick the 60 Hz timers once.
// Returns false if the machine stopped (bad opcode, stack error, ...).
bool runFrame(Chip8& chip8, int cycles);

// Draw the display using half-block characters, two pixel rows per text row,
// so a 64x32 screen fits in 64x16 characters. Lines are separated by '\n'.
std::string renderHalfBlocks(const uint8_t* pixels, int width, int height);

}  // namespace frontend
