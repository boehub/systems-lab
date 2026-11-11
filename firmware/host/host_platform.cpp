// Desktop implementation of platform.h: draws to the terminal.
// Lets the exact same run loop be tested without any hardware.

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include "platform.h"

namespace {

constexpr int kHostFrames = 120;  // about 2 seconds, then the demo exits

int frame = 0;
std::chrono::steady_clock::time_point startTime;
std::vector<uint8_t> lastFrame;

}  // namespace

namespace platform {

void init() { startTime = std::chrono::steady_clock::now(); }

uint32_t millisNow() {
    const auto elapsed = std::chrono::steady_clock::now() - startTime;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
}

void sleepMs(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void drawFrame(const uint8_t* pixels, int width, int height) {
    // Only redraw when something changed (an OLED port would do the same to
    // save bus time).
    const std::vector<uint8_t> current(pixels, pixels + width * height);
    if (current == lastFrame) {
        return;
    }
    lastFrame = current;

    if (isatty(STDOUT_FILENO)) {
        std::fputs("\x1b[H", stdout);  // cursor home: redraw in place
    }
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            std::putchar(pixels[row * width + col] ? '#' : '.');
        }
        std::putchar('\n');
    }
    std::fflush(stdout);
}

uint16_t readKeys() { return 0; }  // no keyboard input on the host yet

bool keepRunning() { return frame++ < kHostFrames; }

}  // namespace platform
