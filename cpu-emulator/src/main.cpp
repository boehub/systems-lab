// Desktop front end for the CHIP-8 core. Runs a ROM in the terminal at a
// steady speed with the 60 Hz timers and keyboard input, or headless.

#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "chip8.h"
#include "frontend.h"
#include "terminal.h"

namespace {

using Clock = std::chrono::steady_clock;

// Set by SIGINT/SIGTERM/SIGHUP so the loop can leave through normal
// destructors, which put the terminal back the way it was.
volatile std::sig_atomic_t g_stopRequested = 0;

void onSignal(int) { g_stopRequested = 1; }

void installSignalHandlers() {
    struct sigaction sa {};
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
}

// The size the interactive view needs: 64 columns, and 16 rows of picture
// plus a status line.
constexpr int kNeedCols = Chip8::kDisplayWidth;
constexpr int kNeedRows = Chip8::kDisplayHeight / 2 + 1;

std::string hex(unsigned value, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*X", digits, value);
    return buf;
}

// Pad or cut a status line to exactly the width of the picture, so a shorter
// message fully overwrites a longer one.
std::string fitToWidth(std::string text) {
    text.resize(kNeedCols, ' ');
    return text;
}

// Print the 64x32 display as '#' and '.' (used by --headless).
void printDisplay(const Chip8& chip8) {
    for (int row = 0; row < Chip8::kDisplayHeight; ++row) {
        for (int col = 0; col < Chip8::kDisplayWidth; ++col) {
            std::putchar(chip8.display[row * Chip8::kDisplayWidth + col] ? '#' : '.');
        }
        std::putchar('\n');
    }
}

std::string stoppedMessage(const Chip8& chip8) {
    return "Stopped: can't run opcode " + hex(chip8.lastOpcode, 4) + " at 0x" +
           hex(chip8.lastPc, 3);
}

// --headless: run a fixed number of instructions with no terminal and no
// waiting, then print the screen. Timers advance on the same schedule as the
// interactive mode (one tick per 60th of a second's worth of instructions).
int runHeadless(Chip8& chip8, const frontend::Options& opts) {
    frontend::Pacer pacer(opts.speed);
    long done = 0;
    bool stopped = false;
    while (done < opts.cycles && !stopped) {
        long n = pacer.nextFrame();
        if (n > opts.cycles - done) n = opts.cycles - done;
        // Count the instructions that actually ran, even if one fails.
        for (long i = 0; i < n; ++i) {
            if (!chip8.cycle()) {
                stopped = true;
                break;
            }
            ++done;
        }
        if (!stopped) chip8.tickTimers();
    }
    printDisplay(chip8);
    if (stopped) {
        std::fprintf(stderr, "%s\n", stoppedMessage(chip8).c_str());
    }
    std::printf("ran %ld cycles, pc = 0x%03X\n", done, chip8.pc);
    return stopped ? 1 : 0;
}

int runInteractive(Chip8& chip8, const frontend::Options& opts) {
    int cols = 0, rows = 0;
    if (!Terminal::size(cols, rows) || cols < kNeedCols || rows < kNeedRows) {
        std::fprintf(stderr,
                     "terminal too small: need at least %dx%d, have %dx%d\n",
                     kNeedCols, kNeedRows, cols, rows);
        return 1;
    }

    installSignalHandlers();
    Terminal term;
    if (!term.ok()) {
        std::fprintf(stderr, "could not set up the terminal\n");
        return 1;
    }

    // One frame every 1/60 s, scheduled from fixed deadlines (not "sleep for
    // 16 ms") so small delays don't accumulate into drift.
    constexpr auto kFrame = std::chrono::nanoseconds(1000000000 / 60);
    const auto start = Clock::now();
    auto next = start;

    frontend::Pacer pacer(opts.speed);
    frontend::KeyHold keys(opts.holdMs);

    const std::string footer = fitToWidth(
        "Esc quits | keypad: 1234 QWER ASDF ZXCV | " + std::to_string(opts.speed) + " Hz");
    std::string status = footer;
    std::string drawnStatus;
    std::array<uint8_t, Chip8::kDisplayWidth * Chip8::kDisplayHeight> drawn{};
    bool haveDrawn = false;
    bool stopped = false;
    bool wasSounding = false;

    while (!g_stopRequested) {
        const auto now = Clock::now();
        const int64_t nowMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();

        // Input.
        if (frontend::handleInput(term.readInput(), keys, nowMs)) break;
        for (int k = 0; k < 16; ++k) {
            chip8.keypad[k] = keys.isDown(k, nowMs) ? 1 : 0;
        }

        // Emulation.
        if (!stopped && !frontend::runFrame(chip8, pacer.nextFrame())) {
            stopped = true;
            status = fitToWidth(stoppedMessage(chip8) + " (Esc to quit)");
        }

        // Sound: a bell when the sound timer starts.
        const bool sounding = chip8.soundTimer > 0;
        if (opts.beep && sounding && !wasSounding) term.write("\a");
        wasSounding = sounding;

        // Drawing: only when something changed.
        if (!haveDrawn || chip8.display != drawn || status != drawnStatus) {
            term.write("\x1b[H" +
                       frontend::renderHalfBlocks(chip8.display.data(), Chip8::kDisplayWidth,
                                                  Chip8::kDisplayHeight) +
                       "\n" + status);
            drawn = chip8.display;
            drawnStatus = status;
            haveDrawn = true;
        }

        // Wait for the next frame. If we've fallen far behind (the machine
        // was suspended, say), skip ahead instead of racing to catch up.
        next += kFrame;
        std::this_thread::sleep_until(next);
        if (Clock::now() > next + 5 * kFrame) next = Clock::now();
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    frontend::Options opts;
    std::string error;
    if (!frontend::parseArgs(argc, argv, opts, error)) {
        std::fprintf(stderr, "chip8: %s\n\n%s", error.c_str(), frontend::usage());
        return 2;
    }
    if (opts.help) {
        std::fputs(frontend::usage(), stdout);
        return 0;
    }

    std::ifstream file(opts.rom, std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "could not open ROM: %s\n", opts.rom.c_str());
        return 1;
    }
    const std::vector<uint8_t> rom((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());

    Chip8 chip8;
    chip8.quirks = opts.quirks;
    if (opts.seed != 0) {
        chip8.seedRandom(opts.seed);
    } else if (!opts.headless) {
        // Interactive runs should differ from one to the next. (Headless runs
        // keep the core's fixed default so output is repeatable.)
        chip8.seedRandom(static_cast<uint32_t>(Clock::now().time_since_epoch().count()));
    }
    if (!chip8.loadRom(rom.data(), rom.size())) {
        std::fprintf(stderr, "ROM too large: %s\n", opts.rom.c_str());
        return 1;
    }

    bool headless = opts.headless;
    if (!headless && !(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO))) {
        std::fprintf(stderr, "chip8: not a terminal, running headless\n");
        headless = true;
    }
    return headless ? runHeadless(chip8, opts) : runInteractive(chip8, opts);
}
