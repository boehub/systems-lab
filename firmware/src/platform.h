#pragma once

#include <cstdint>

// Everything the emulator needs from the outside world. The run loop in
// main.cpp only talks to hardware through these functions, so porting to a
// new board means implementing this file once:
//
//   host/host_platform.cpp   terminal + desktop clock (works today)
//   pico/pico_platform.cpp   Raspberry Pi Pico: OLED, button matrix, buzzer
namespace platform {

// Set up display, buttons, and clock.
void init();

// Milliseconds since startup.
uint32_t millisNow();

// Wait for `ms` milliseconds.
void sleepMs(uint32_t ms);

// Show one frame. `pixels` is width*height bytes, row by row, 0 = off, 1 = on.
void drawFrame(const uint8_t* pixels, int width, int height);

// Keypad state: bit i set means CHIP-8 key i (0x0..0xF) is held down.
uint16_t readKeys();

// Turn the buzzer on or off (the CHIP-8 sound timer is running). The host
// build ignores this.
void setBuzzer(bool on);

// True while the MENU button is held (an extra button, separate from the
// 16-key pad). Returns to the program menu; with a single program built in, it
// restarts that program. The host build has no such button.
bool menuButton();

// A number that differs from one boot to the next, to seed the random number
// generator (CXNN). Hardware uses its noise source; the host uses the clock.
uint32_t randomSeed();

// Return false to stop the run loop. Hardware returns true forever; the host
// build stops after a couple of seconds so it can be run as a demo.
bool keepRunning();

}  // namespace platform
