#pragma once

// The program-selection menu. Hardware-independent: it is given the keys that
// are held and draws into an ordinary pixel buffer, so it is unit-tested on
// the desktop.
//
//   keypad 2 = up, 8 = down, 5 = run the highlighted program

#include <cstdint>

#include "rom_entry.h"

class Menu {
public:
    static constexpr int kWidth = 128;
    static constexpr int kHeight = 64;
    static constexpr int kVisible = 6;  // entries on screen at once

    Menu(const RomEntry* roms, int count) : roms_(roms), count_(count) {}

    // Show the menu. `heldKeys` are keys already down; they don't count as
    // presses until released and pressed again (so the key that opened the
    // menu doesn't also choose something).
    void open(uint16_t heldKeys);

    // Feed the current key state once per frame. Returns the index of the
    // program to run when one has been chosen, otherwise -1.
    int update(uint16_t keys);

    // Draw the menu into a kWidth x kHeight pixel buffer (1 = lit).
    void render(uint8_t* pixels) const;

    int cursor() const { return cursor_; }
    int firstVisible() const { return top_; }

private:
    const RomEntry* roms_;
    int count_;
    int cursor_ = 0;
    int top_ = 0;
    uint16_t previous_ = 0;
};
