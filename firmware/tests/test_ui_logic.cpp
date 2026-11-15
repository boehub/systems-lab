// Tests for the hardware-independent UI pieces: the 5x7 text, the program menu,
// the 60 Hz frame clock and the key gate.
//
//   make test

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "frame_clock.h"
#include "key_gate.h"
#include "menu.h"
#include "text.h"

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

// ---- text -------------------------------------------------------------------

// Draw one string into a small image and return it as rows of '#' and '.'.
static std::vector<std::string> picture(const char* s, int w = 40, int h = 9) {
    std::vector<uint8_t> px(w * h, 0);
    text::draw(px.data(), w, h, 1, 1, s);
    std::vector<std::string> rows;
    for (int y = 0; y < h; ++y) {
        std::string r;
        for (int x = 0; x < w; ++x) r += px[y * w + x] ? '#' : '.';
        rows.push_back(r);
    }
    return rows;
}

static void testTextShapes() {
    // 'A' sits at (1,1): row 1 is the top (".###."), row 4 the crossbar.
    auto a = picture("A");
    CHECK(a[1].substr(1, 5) == ".###.");
    CHECK(a[2].substr(1, 5) == "#...#");
    CHECK(a[4].substr(1, 5) == "#####");
    CHECK(a[7].substr(1, 5) == "#...#");
    CHECK(a[0] == std::string(40, '.') && a[8] == std::string(40, '.'));

    // The second character starts one advance (6 px) later.
    auto ab = picture("AB");
    CHECK(ab[1].substr(7, 5) == "####.");
    CHECK(ab[1].substr(1, 5) == ".###.");

    // Lower case is drawn as upper case; unknown characters become '?'.
    CHECK(picture("a") == picture("A"));
    CHECK(picture("~") == picture("?"));
    CHECK(picture("?") != picture("O"));

    // Spaces draw nothing.
    auto sp = picture("   ");
    for (const auto& r : sp) CHECK(r == std::string(40, '.'));

    CHECK(text::width("") == 0);
    CHECK(text::width("A") == 5);
    CHECK(text::width("AB") == 11);
    CHECK(text::width("ABCDEF", 3) == 17);  // only the first 3 count
}

static void testGlyphsAreDistinct() {
    // Every character in the menu's alphabet looks different from every other,
    // which catches a copy-and-paste slip in the font table.
    const char* all = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-_.:/<>?";
    const int n = static_cast<int>(std::strlen(all));
    for (int i = 0; i < n; ++i) {
        const std::string one(1, all[i]);
        for (int j = i + 1; j < n; ++j) {
            const std::string two(1, all[j]);
            CHECK(picture(one.c_str()) != picture(two.c_str()));
        }
    }
}

static void testTextClipping() {
    // Drawing partly or wholly outside the image must not write out of bounds
    // (the guard bytes around the buffer would change).
    std::vector<uint8_t> buf(10 * 10 + 20, 0xAA);
    uint8_t* px = buf.data() + 10;
    std::memset(px, 0, 100);
    text::draw(px, 10, 10, -3, -3, "WWW");
    text::draw(px, 10, 10, 8, 8, "WWW");
    text::draw(px, 10, 10, 100, 100, "WWW");
    text::fillRect(px, 10, 10, -5, -5, 30, 30, 1);
    bool guardsIntact = true;
    for (int i = 0; i < 10; ++i) guardsIntact = guardsIntact && buf[i] == 0xAA;
    for (int i = 110; i < 120; ++i) guardsIntact = guardsIntact && buf[i] == 0xAA;
    CHECK(guardsIntact);
    CHECK(px[0] == 1 && px[99] == 1);  // the fill covered it all
}

// ---- menu -------------------------------------------------------------------

static const uint8_t kData[] = {0x12, 0x00};
static const RomEntry kRoms[] = {
    {"ALPHA", kData, 2}, {"BRAVO", kData, 2}, {"CHARLIE", kData, 2}, {"DELTA", kData, 2},
    {"ECHO", kData, 2},  {"FOXTROT", kData, 2}, {"GOLF", kData, 2},  {"HOTEL", kData, 2},
};
static const int kCount = 8;

static const uint16_t UP = 1u << 2, DOWN = 1u << 8, SELECT = 1u << 5;

// Press a key for one frame, then release it.
static int tap(Menu& m, uint16_t key) {
    const int r = m.update(key);
    m.update(0);
    return r;
}

static void testMenuNavigation() {
    Menu m(kRoms, kCount);
    m.open(0);
    CHECK(m.cursor() == 0);
    CHECK(tap(m, DOWN) == -1 && m.cursor() == 1);
    CHECK(tap(m, DOWN) == -1 && m.cursor() == 2);
    CHECK(tap(m, UP) == -1 && m.cursor() == 1);
    CHECK(tap(m, SELECT) == 1);          // choosing returns the index

    // Wraps at both ends.
    Menu w(kRoms, kCount);
    w.open(0);
    tap(w, UP);
    CHECK(w.cursor() == kCount - 1);
    tap(w, DOWN);
    CHECK(w.cursor() == 0);

    // Holding a key is one press, not repeated moves.
    Menu h(kRoms, kCount);
    h.open(0);
    h.update(DOWN);
    h.update(DOWN);
    h.update(DOWN);
    CHECK(h.cursor() == 1);
    h.update(0);
    h.update(DOWN);
    CHECK(h.cursor() == 2);

    // A key already held when the menu opens doesn't count until pressed again.
    Menu o(kRoms, kCount);
    o.open(SELECT);
    CHECK(o.update(SELECT) == -1);
    o.update(0);
    CHECK(o.update(SELECT) == 0);

    // Other keys do nothing; no entries never picks anything.
    Menu n(kRoms, kCount);
    n.open(0);
    CHECK(tap(n, 1u << 7) == -1 && n.cursor() == 0);
    Menu empty(kRoms, 0);
    empty.open(0);
    CHECK(tap(empty, SELECT) == -1 && tap(empty, DOWN) == -1);
}

static void testMenuScrolling() {
    Menu m(kRoms, kCount);
    m.open(0);
    CHECK(m.firstVisible() == 0);
    for (int i = 0; i < 5; ++i) tap(m, DOWN);   // cursor 5: still on screen
    CHECK(m.cursor() == 5 && m.firstVisible() == 0);
    tap(m, DOWN);                               // cursor 6: scrolls by one
    CHECK(m.cursor() == 6 && m.firstVisible() == 1);
    tap(m, DOWN);
    CHECK(m.cursor() == 7 && m.firstVisible() == 2);
    tap(m, DOWN);                               // wraps to the top
    CHECK(m.cursor() == 0 && m.firstVisible() == 0);
    tap(m, UP);                                 // and back to the bottom
    CHECK(m.cursor() == 7 && m.firstVisible() == 2);
    for (int i = 0; i < 6; ++i) tap(m, UP);
    CHECK(m.cursor() == 1 && m.firstVisible() == 1);
    tap(m, UP);
    CHECK(m.cursor() == 0 && m.firstVisible() == 0);
}

static bool lit(const std::vector<uint8_t>& px, int x, int y) {
    return px[y * Menu::kWidth + x] != 0;
}

// Does the text appear (lit) at (x, y)? Compared pixel by pixel with text::draw.
static bool textAt(const std::vector<uint8_t>& px, int x, int y, const char* s, bool dark) {
    std::vector<uint8_t> ref(Menu::kWidth * Menu::kHeight, dark ? 1 : 0);
    text::draw(ref.data(), Menu::kWidth, Menu::kHeight, x, y, s, dark ? 0 : 1);
    for (int yy = y; yy < y + 7; ++yy)
        for (int xx = x; xx < x + text::width(s); ++xx)
            if (lit(px, xx, yy) != (ref[yy * Menu::kWidth + xx] != 0)) return false;
    return true;
}

static void testMenuRender() {
    Menu m(kRoms, kCount);
    m.open(0);
    std::vector<uint8_t> px(Menu::kWidth * Menu::kHeight);
    m.render(px.data());

    CHECK(textAt(px, 2, 1, "PROGRAMS", false));              // title
    CHECK(textAt(px, Menu::kWidth - 2 - text::width("1/8"), 1, "1/8", false));
    for (int x = 0; x < Menu::kWidth; ++x) CHECK(lit(px, x, 8));  // the rule

    // The selected row is a lit bar with dark text; the others are plain.
    CHECK(lit(px, 0, 10) && lit(px, 127, 18) && !lit(px, 0, 19));
    CHECK(textAt(px, 3, 11, "ALPHA", true));
    CHECK(textAt(px, 3, 20, "BRAVO", false));
    CHECK(textAt(px, 3, 29, "CHARLIE", false));
    CHECK(textAt(px, 3, 56, "FOXTROT", false));              // 6th visible row

    // After scrolling to the bottom, the first rows are gone and the cursor row is lit.
    for (int i = 0; i < 7; ++i) tap(m, DOWN);
    m.render(px.data());
    CHECK(textAt(px, Menu::kWidth - 2 - text::width("8/8"), 1, "8/8", false));
    CHECK(textAt(px, 3, 10 + 5 * 9 + 1, "HOTEL", true));     // last row, selected
    CHECK(textAt(px, 3, 11, "CHARLIE", false));              // top row now entry 3

    // Rendering twice gives the same picture (no leftovers).
    std::vector<uint8_t> again(px.size(), 1);
    m.render(again.data());
    CHECK(again == px);
}

static void testLongNamesAreCut() {
    static const RomEntry longName[] = {{"ABCDEFGHIJKLMNOPQRSTUVWXYZ", kData, 2}};
    Menu m(longName, 1);
    m.open(0);
    std::vector<uint8_t> px(Menu::kWidth * Menu::kHeight);
    m.render(px.data());
    // 19 characters (3 + 19*6 - 1 = 116 px) fit; nothing is drawn past the edge.
    bool pastEdge = false;
    for (int y = 10; y < 19; ++y)
        for (int x = 118; x < 128; ++x)
            if (!lit(px, x, y)) pastEdge = true;  // text would punch dark pixels in the bar
    CHECK(!pastEdge);
}

// ---- frame clock ------------------------------------------------------------

// Run the loop as the firmware does, with a pretend clock, and return the time
// at the start of frame `n` (frame 0 starts at `start`).
static uint32_t timeOfFrame(uint32_t start, int n, uint32_t workMs) {
    FrameClock clock(start);
    uint32_t now = start;
    for (int i = 0; i < n; ++i) {
        now += workMs;                              // this frame's work
        now += clock.msUntilNextFrame(now);         // then sleep
    }
    return now;
}

static void testFrameClock() {
    // 60 frames take exactly one second; 3600 take exactly a minute.
    CHECK(timeOfFrame(0, 60, 1) - 0 == 1000);
    CHECK(timeOfFrame(500, 3600, 2) - 500 == 60000);
    CHECK(timeOfFrame(0, 6000, 0) == 100000);

    // Frames are 16 or 17 ms apart, never anything else.
    FrameClock c(0);
    uint32_t now = 0, last = 0;
    bool spacingOk = true;
    for (int i = 0; i < 120; ++i) {
        now += c.msUntilNextFrame(now);
        const uint32_t gap = now - last;
        spacingOk = spacingOk && (gap == 16 || gap == 17);
        last = now;
    }
    CHECK(spacingOk);

    // Work that takes longer than a frame: no sleeping, and it catches up.
    FrameClock slow(0);
    CHECK(slow.msUntilNextFrame(40) == 0);   // 40 ms into a 16 ms frame: late

    // Far behind (a long stall): start over from now instead of racing.
    FrameClock stalled(0);
    CHECK(stalled.msUntilNextFrame(5000) == 0);
    CHECK(stalled.msUntilNextFrame(5001) == 15);  // normal pacing again: due at 5016

    // The 32-bit clock wrapping around (after 49 days) is harmless.
    const uint32_t nearWrap = 0xFFFFFF00u;
    CHECK(timeOfFrame(nearWrap, 600, 3) - nearWrap == 10000);
}

// ---- key gate ---------------------------------------------------------------

static void testKeyGate() {
    KeyGate g;
    CHECK(g.filter(0x0020) == 0x0020);       // nothing hidden at first

    g.begin(0x0020);                         // key 5 was held at launch
    CHECK(g.filter(0x0020) == 0);            // hidden while still held
    CHECK(g.filter(0x0020 | 0x0001) == 0x0001);  // other keys pass
    CHECK(g.filter(0) == 0);                 // released...
    CHECK(g.filter(0x0020) == 0x0020);       // ...so a new press is seen

    g.begin(0);
    CHECK(g.filter(0xFFFF) == 0xFFFF);
}

int main() {
    testTextShapes();
    testGlyphsAreDistinct();
    testTextClipping();
    testMenuNavigation();
    testMenuScrolling();
    testMenuRender();
    testLongNamesAreCut();
    testFrameClock();
    testKeyGate();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
