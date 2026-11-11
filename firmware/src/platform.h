#pragma once

#include <cstdint>

// Everything the emulator needs from the outside world. The run loop in
// main.cpp only talks to hardware through these functions, so porting to a
// new board means implementing this file once:
//
//   host/host_platform.cpp   terminal + desktop clock (works today)
//   (future) pico/...        OLED display, GPIO buttons, hardware timer
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

// Return false to stop the run loop. Hardware returns true forever; the host
// build stops after a couple of seconds so it can be run as a demo.
bool keepRunning();

}  // namespace platform
