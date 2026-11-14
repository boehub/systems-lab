#include "frontend.h"

#include <cstdlib>
#include <cstring>

namespace frontend {

namespace {

// Parse a whole-number option value in [lo, hi].
bool parseInt(const char* text, long lo, long hi, long& out) {
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (*end != '\0' || v < lo || v > hi) return false;
    out = v;
    return true;
}

}  // namespace

const char* usage() {
    return "usage: chip8 [options] <rom>\n"
           "\n"
           "Runs a CHIP-8 ROM in the terminal. Keypad (left side of a QWERTY\n"
           "keyboard):  1 2 3 4 / Q W E R / A S D F / Z X C V.  Esc quits.\n"
           "\n"
           "options:\n"
           "  --speed N        instructions per second (default 700)\n"
           "  --hold N         ms a key press counts as held (default 150)\n"
           "  --beep           ring the terminal bell while the sound timer runs\n"
           "  --seed N         random seed (default: from the clock)\n"
           "  --quirk-shift    8XY6/8XYE shift VY into VX (original COSMAC VIP)\n"
           "  --quirk-jump     BNNN jumps to NNN + VX (CHIP-48 / SUPER-CHIP)\n"
           "  --quirk-loadstore  FX55/FX65 leave I advanced (original COSMAC VIP)\n"
           "  --quirk-vfreset  8XY1/8XY2/8XY3 set VF to 0 (original COSMAC VIP)\n"
           "  --quirk-displaywait  DXYN draws at most once per frame (COSMAC VIP)\n"
           "  --vip            all four quirks above that the COSMAC VIP had\n"
           "  --headless       no terminal: run N instructions, print the screen\n"
           "  --cycles N       instructions to run with --headless (default 1000)\n"
           "  -h, --help       show this help\n"
           "\n"
           "Without a terminal (for example when piped), --headless is implied.\n";
}

bool parseArgs(int argc, const char* const* argv, Options& out,
               std::string& error) {
    out = Options();
    bool haveRom = false;
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        // Options that take a value read the next argument.
        auto takeInt = [&](const char* name, long lo, long hi, long& value) {
            if (i + 1 >= argc || !parseInt(argv[i + 1], lo, hi, value)) {
                error = std::string(name) + " needs a whole number from " +
                        std::to_string(lo) + " to " + std::to_string(hi);
                return false;
            }
            ++i;
            return true;
        };
        long v = 0;
        if (!std::strcmp(a, "-h") || !std::strcmp(a, "--help")) {
            out.help = true;
        } else if (!std::strcmp(a, "--speed")) {
            if (!takeInt("--speed", 1, 100000, v)) return false;
            out.speed = static_cast<int>(v);
        } else if (!std::strcmp(a, "--hold")) {
            if (!takeInt("--hold", 1, 2000, v)) return false;
            out.holdMs = static_cast<int>(v);
        } else if (!std::strcmp(a, "--cycles")) {
            if (!takeInt("--cycles", 1, 1000000000L, v)) return false;
            out.cycles = v;
        } else if (!std::strcmp(a, "--seed")) {
            if (!takeInt("--seed", 1, 4294967295L, v)) return false;
            out.seed = static_cast<uint32_t>(v);
        } else if (!std::strcmp(a, "--beep")) {
            out.beep = true;
        } else if (!std::strcmp(a, "--headless")) {
            out.headless = true;
        } else if (!std::strcmp(a, "--quirk-shift")) {
            out.quirks.shiftUsesVy = true;
        } else if (!std::strcmp(a, "--quirk-jump")) {
            out.quirks.jumpUsesVx = true;
        } else if (!std::strcmp(a, "--quirk-loadstore")) {
            out.quirks.loadStoreIncrementsI = true;
        } else if (!std::strcmp(a, "--quirk-vfreset")) {
            out.quirks.logicResetsVf = true;
        } else if (!std::strcmp(a, "--quirk-displaywait")) {
            out.quirks.displayWait = true;
        } else if (!std::strcmp(a, "--vip")) {
            // Everything the original COSMAC VIP did differently.
            out.quirks.shiftUsesVy = true;
            out.quirks.loadStoreIncrementsI = true;
            out.quirks.logicResetsVf = true;
            out.quirks.displayWait = true;
        } else if (a[0] == '-' && a[1] != '\0') {
            error = std::string("unknown option: ") + a;
            return false;
        } else {
            if (haveRom) {
                error = "only one ROM can be given";
                return false;
            }
            out.rom = a;
            haveRom = true;
        }
    }
    if (!haveRom && !out.help) {
        error = "no ROM given";
        return false;
    }
    return true;
}

int keyFromChar(char c) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    switch (c) {
        case '1': return 0x1;
        case '2': return 0x2;
        case '3': return 0x3;
        case '4': return 0xC;
        case 'q': return 0x4;
        case 'w': return 0x5;
        case 'e': return 0x6;
        case 'r': return 0xD;
        case 'a': return 0x7;
        case 's': return 0x8;
        case 'd': return 0x9;
        case 'f': return 0xE;
        case 'z': return 0xA;
        case 'x': return 0x0;
        case 'c': return 0xB;
        case 'v': return 0xF;
        default: return -1;
    }
}

bool handleInput(const std::string& bytes, KeyHold& keys, int64_t nowMs) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        if (c == '\x1b') {
            // Arrow keys and friends arrive as ESC [ ... or ESC O ...; skip
            // the whole sequence. An ESC with nothing after it is the Esc key.
            if (i + 1 < bytes.size() && (bytes[i + 1] == '[' || bytes[i + 1] == 'O')) {
                i += 2;
                while (i < bytes.size() &&
                       !(bytes[i] >= '@' && bytes[i] <= '~')) {
                    ++i;
                }
                continue;
            }
            return true;
        }
        const int key = keyFromChar(c);
        if (key >= 0) keys.press(key, nowMs);
    }
    return false;
}

bool runFrame(Chip8& chip8, int cycles) {
    for (int i = 0; i < cycles; ++i) {
        if (!chip8.cycle()) return false;
    }
    chip8.tickTimers();
    return true;
}

std::string renderHalfBlocks(const uint8_t* pixels, int width, int height) {
    // UTF-8 for U+2580 (upper half), U+2584 (lower half), U+2588 (full block).
    static const char* const kUpper = "\xE2\x96\x80";
    static const char* const kLower = "\xE2\x96\x84";
    static const char* const kFull = "\xE2\x96\x88";

    std::string out;
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; ++x) {
            const bool top = pixels[y * width + x] != 0;
            const bool bottom = (y + 1 < height) && pixels[(y + 1) * width + x] != 0;
            if (top && bottom) {
                out += kFull;
            } else if (top) {
                out += kUpper;
            } else if (bottom) {
                out += kLower;
            } else {
                out += ' ';
            }
        }
        if (y + 2 < height) out += '\n';
    }
    return out;
}

}  // namespace frontend
