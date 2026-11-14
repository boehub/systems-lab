// Tests for the OS-free front-end logic (src/frontend.*).
//
//   make test

#include <cstdio>
#include <string>
#include <vector>

#include "chip8.h"
#include "frontend.h"

using namespace frontend;

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

// Parse a command line given as a list of words (after the program name).
static bool parse(std::vector<const char*> words, Options& opts, std::string& err) {
    words.insert(words.begin(), "chip8");
    return parseArgs(static_cast<int>(words.size()), words.data(), opts, err);
}

static void testOptionDefaults() {
    Options o;
    std::string err;
    CHECK(parse({"game.ch8"}, o, err));
    CHECK(o.rom == "game.ch8");
    CHECK(o.speed == 700);
    CHECK(o.holdMs == 150);
    CHECK(!o.headless && !o.beep && !o.help);
    CHECK(o.cycles == 1000);
    CHECK(o.seed == 0);
    CHECK(!o.quirks.shiftUsesVy && !o.quirks.jumpUsesVx &&
          !o.quirks.loadStoreIncrementsI);
}

static void testOptionValues() {
    Options o;
    std::string err;
    CHECK(parse({"--speed", "1200", "--hold", "80", "--beep", "--seed", "42",
                 "--quirk-shift", "--quirk-jump", "--quirk-loadstore",
                 "--headless", "--cycles", "5000", "x.ch8"},
                o, err));
    CHECK(o.speed == 1200 && o.holdMs == 80 && o.beep && o.seed == 42);
    CHECK(o.quirks.shiftUsesVy && o.quirks.jumpUsesVx && o.quirks.loadStoreIncrementsI);
    CHECK(o.headless && o.cycles == 5000);
    CHECK(o.rom == "x.ch8");

    // The two newer quirk flags.
    CHECK(parse({"--quirk-vfreset", "x.ch8"}, o, err));
    CHECK(o.quirks.logicResetsVf && !o.quirks.displayWait);
    CHECK(parse({"--quirk-displaywait", "x.ch8"}, o, err));
    CHECK(o.quirks.displayWait && !o.quirks.logicResetsVf);

    // --vip turns on the four original-hardware quirks but not BNNN's.
    CHECK(parse({"--vip", "x.ch8"}, o, err));
    CHECK(o.quirks.shiftUsesVy && o.quirks.loadStoreIncrementsI &&
          o.quirks.logicResetsVf && o.quirks.displayWait);
    CHECK(!o.quirks.jumpUsesVx);

    // The ROM may come before the options.
    CHECK(parse({"x.ch8", "--speed", "300"}, o, err));
    CHECK(o.speed == 300 && o.rom == "x.ch8");
}

static void testOptionErrors() {
    Options o;
    std::string err;
    CHECK(!parse({}, o, err));
    CHECK(err.find("no ROM") != std::string::npos);

    CHECK(!parse({"--speed", "x.ch8"}, o, err));  // value isn't a number
    CHECK(err.find("--speed") != std::string::npos);
    CHECK(!parse({"--speed"}, o, err));           // missing value
    CHECK(!parse({"--speed", "0", "x.ch8"}, o, err));
    CHECK(!parse({"--speed", "100001", "x.ch8"}, o, err));
    CHECK(!parse({"--hold", "5000", "x.ch8"}, o, err));
    CHECK(!parse({"--seed", "0", "x.ch8"}, o, err));
    CHECK(!parse({"--speed", "12abc", "x.ch8"}, o, err));
    CHECK(!parse({"--bogus", "x.ch8"}, o, err));
    CHECK(err.find("--bogus") != std::string::npos);
    CHECK(!parse({"a.ch8", "b.ch8"}, o, err));

    // --help works without a ROM.
    CHECK(parse({"--help"}, o, err));
    CHECK(o.help);
}

static void testKeyboardLayout() {
    // The four rows of the QWERTY keyboard map to the CHIP-8 keypad.
    const char* rows = "1234qwerasdfzxcv";
    const int expected[16] = {0x1, 0x2, 0x3, 0xC, 0x4, 0x5, 0x6, 0xD,
                              0x7, 0x8, 0x9, 0xE, 0xA, 0x0, 0xB, 0xF};
    for (int i = 0; i < 16; ++i) {
        CHECK(keyFromChar(rows[i]) == expected[i]);
    }
    CHECK(keyFromChar('Q') == 0x4);  // case doesn't matter
    CHECK(keyFromChar('V') == 0xF);
    CHECK(keyFromChar('p') == -1);
    CHECK(keyFromChar(' ') == -1);
    CHECK(keyFromChar('5') == -1);

    // Every one of the 16 keys is reachable exactly once.
    int seen = 0;
    for (int i = 0; i < 16; ++i) seen |= 1 << keyFromChar(rows[i]);
    CHECK(seen == 0xFFFF);
}

static void testKeyHoldWindow() {
    KeyHold keys(150);
    CHECK(!keys.isDown(5, 0));
    keys.press(5, 1000);
    CHECK(keys.isDown(5, 1000));
    CHECK(keys.isDown(5, 1149));
    CHECK(!keys.isDown(5, 1150));  // window is over
    CHECK(!keys.isDown(6, 1000));  // other keys unaffected

    keys.press(5, 1100);  // auto-repeat extends the window
    CHECK(keys.isDown(5, 1249));
    CHECK(!keys.isDown(5, 1250));
}

static void testHandleInput() {
    KeyHold keys(150);
    CHECK(!handleInput("q", keys, 0));
    CHECK(keys.isDown(0x4, 10));

    CHECK(!handleInput("x1", keys, 0));  // several keys in one read
    CHECK(keys.isDown(0x0, 10) && keys.isDown(0x1, 10));

    KeyHold k2(150);
    CHECK(handleInput("\x1b", k2, 0));  // lone Esc quits
    CHECK(handleInput("q\x1b", k2, 0));

    // Arrow keys and function keys are escape sequences: ignore, don't quit.
    KeyHold k3(150);
    CHECK(!handleInput("\x1b[A", k3, 0));
    CHECK(!handleInput("\x1b[1;5C", k3, 0));
    CHECK(!handleInput("\x1bOP", k3, 0));
    // ...and the bytes inside them must not count as key presses.
    CHECK(!handleInput("\x1b[A", k3, 0));
    for (int k = 0; k < 16; ++k) CHECK(!k3.isDown(k, 0));

    // A sequence followed by a real key press still registers the key.
    KeyHold k4(150);
    CHECK(!handleInput("\x1b[Aq", k4, 0));
    CHECK(k4.isDown(0x4, 10));
}

static void testPacer() {
    // Over 60 frames the instruction count is exactly the speed.
    for (int speed : {700, 500, 1, 59, 60, 61, 1000, 1234}) {
        Pacer p(speed);
        int total = 0;
        int minPerFrame = 1 << 30, maxPerFrame = 0;
        for (int f = 0; f < 60; ++f) {
            const int n = p.nextFrame();
            total += n;
            if (n < minPerFrame) minPerFrame = n;
            if (n > maxPerFrame) maxPerFrame = n;
        }
        CHECK(total == speed);
        CHECK(maxPerFrame - minPerFrame <= 1);  // evenly spread
    }
    // And it stays exact over many seconds.
    Pacer p(700);
    long total = 0;
    for (int f = 0; f < 60 * 100; ++f) total += p.nextFrame();
    CHECK(total == 70000);
}

static void testRunFrame() {
    Chip8 c;
    // 0x200: V0 += 1, then jump back; so each cycle adds 1 every two cycles.
    c.memory[0x200] = 0x70; c.memory[0x201] = 0x01;
    c.memory[0x202] = 0x12; c.memory[0x203] = 0x00;
    c.delayTimer = 5;
    c.soundTimer = 3;
    CHECK(runFrame(c, 10));
    CHECK(c.V[0] == 5);          // 10 instructions = 5 loops
    CHECK(c.delayTimer == 4);    // one tick per frame
    CHECK(c.soundTimer == 2);

    CHECK(runFrame(c, 0));       // an empty frame still ticks timers
    CHECK(c.delayTimer == 3);

    Chip8 bad;
    bad.memory[0x200] = 0xFF; bad.memory[0x201] = 0xFF;
    bad.delayTimer = 5;
    CHECK(!runFrame(bad, 10));
    CHECK(bad.delayTimer == 5);  // a stopped machine doesn't tick
}

static void testRenderHalfBlocks() {
    const std::string U = "\xE2\x96\x80", L = "\xE2\x96\x84", F = "\xE2\x96\x88";

    // 4 wide, 4 tall -> 2 text rows.
    // row0: 1 1 0 0
    // row1: 1 0 1 0   -> top row of text:  full, upper, lower, blank
    // row2: 0 0 0 0
    // row3: 0 1 0 0   -> bottom row:       blank, lower, blank, blank
    const uint8_t px[16] = {1, 1, 0, 0,  1, 0, 1, 0,  0, 0, 0, 0,  0, 1, 0, 0};
    const std::string got = renderHalfBlocks(px, 4, 4);
    CHECK(got == F + U + L + " " + "\n" + " " + L + "  ");

    // A full 64x32 screen is 16 lines of 64 characters.
    std::vector<uint8_t> blank(64 * 32, 0);
    const std::string s = renderHalfBlocks(blank.data(), 64, 32);
    int lines = 1;
    for (char c : s) lines += (c == '\n');
    CHECK(lines == 16);
    CHECK(s.size() == 16 * 64 + 15);  // spaces are 1 byte each, plus newlines

    // All lit: every cell is a full block.
    std::vector<uint8_t> lit(64 * 32, 1);
    const std::string full = renderHalfBlocks(lit.data(), 64, 32);
    CHECK(full.substr(0, 3) == F);
}

int main() {
    testOptionDefaults();
    testOptionValues();
    testOptionErrors();
    testKeyboardLayout();
    testKeyHoldWindow();
    testHandleInput();
    testPacer();
    testRunFrame();
    testRenderHalfBlocks();

    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
