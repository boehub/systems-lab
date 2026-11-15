// Tests for the parts of the Pico port that don't touch hardware: packing
// frames for the OLED, decoding the button matrix, debouncing.
//
//   make test

#include <cstdio>
#include <cstring>
#include <vector>

#include "keypad_matrix.h"
#include "oled_frame.h"

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

// Is the OLED pixel at (x, y) lit in a packed buffer?
static bool lit(const uint8_t* buf, int x, int y) {
    return (buf[(y / 8) * oled::kWidth + x] >> (y % 8)) & 1;
}

static void testPackSinglePixel() {
    // One lit CHIP-8 pixel becomes a 2x2 block; nothing else is lit.
    std::vector<uint8_t> px(64 * 32, 0);
    px[5 * 64 + 10] = 1;  // x=10, y=5
    uint8_t buf[oled::kBufferSize];
    oled::pack(px.data(), 64, 32, buf);

    int count = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 128; ++x) count += lit(buf, x, y);
    CHECK(count == 4);
    CHECK(lit(buf, 20, 10) && lit(buf, 21, 10) && lit(buf, 20, 11) && lit(buf, 21, 11));
}

static void testPackCornersAndPages() {
    std::vector<uint8_t> px(64 * 32, 0);
    px[0] = 1;                  // top-left
    px[63] = 1;                 // top-right
    px[31 * 64] = 1;            // bottom-left
    px[31 * 64 + 63] = 1;       // bottom-right
    uint8_t buf[oled::kBufferSize];
    oled::pack(px.data(), 64, 32, buf);

    CHECK(lit(buf, 0, 0) && lit(buf, 1, 1));
    CHECK(lit(buf, 126, 0) && lit(buf, 127, 1));
    CHECK(lit(buf, 0, 62) && lit(buf, 1, 63));
    CHECK(lit(buf, 126, 62) && lit(buf, 127, 63));
    CHECK(!lit(buf, 2, 0) && !lit(buf, 0, 2));

    // Page layout: the top-left block is in page 0, bits 0 and 1 of columns 0 and 1.
    CHECK(buf[0] == 0x03 && buf[1] == 0x03);
    // The bottom-left block is in page 7, bits 6 and 7.
    CHECK(buf[7 * 128] == 0xC0 && buf[7 * 128 + 1] == 0xC0);
}

static void testPackFullAndBlank() {
    std::vector<uint8_t> on(64 * 32, 1), off(64 * 32, 0);
    uint8_t buf[oled::kBufferSize];
    oled::pack(on.data(), 64, 32, buf);
    bool allOn = true;
    for (uint8_t b : buf) allOn = allOn && b == 0xFF;
    CHECK(allOn);
    oled::pack(off.data(), 64, 32, buf);
    bool allOff = true;
    for (uint8_t b : buf) allOff = allOff && b == 0;
    CHECK(allOff);
}

static void testPackOtherSizes() {
    // A smaller image is centred (not stretched to the edge)...
    std::vector<uint8_t> small(32 * 16, 1);
    uint8_t buf[oled::kBufferSize];
    oled::pack(small.data(), 32, 16, buf);  // scale 4 -> 128x64, fills
    CHECK(lit(buf, 0, 0) && lit(buf, 127, 63));

    std::vector<uint8_t> tiny(10 * 10, 1);  // scale 6 -> 60x60, centred
    oled::pack(tiny.data(), 10, 10, buf);
    CHECK(!lit(buf, 0, 0) && lit(buf, 34, 2) && lit(buf, 93, 61) && !lit(buf, 94, 30));

    // ...and an image bigger than the display is cropped, never overflowing.
    std::vector<uint8_t> big(200 * 100, 1);
    oled::pack(big.data(), 200, 100, buf);
    CHECK(lit(buf, 0, 0) && lit(buf, 127, 63));
}

static void testChangedPages() {
    uint8_t a[oled::kBufferSize] = {0}, b[oled::kBufferSize] = {0};
    CHECK(oled::changedPages(a, b) == 0);
    b[0] = 1;                       // page 0
    CHECK(oled::changedPages(a, b) == 0x01);
    b[3 * 128 + 127] = 9;           // page 3, last column
    b[7 * 128 + 5] = 1;             // page 7
    CHECK(oled::changedPages(a, b) == (0x01 | 0x08 | 0x80));
    std::memcpy(a, b, sizeof a);
    CHECK(oled::changedPages(a, b) == 0);
}

static void testMatrixLayout() {
    // Pressing one button at a time gives exactly that CHIP-8 key.
    const uint8_t expectedKey[4][4] = {{1, 2, 3, 12}, {4, 5, 6, 13}, {7, 8, 9, 14}, {10, 0, 11, 15}};
    uint16_t seen = 0;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            uint8_t cols[4] = {0, 0, 0, 0};
            cols[r] = static_cast<uint8_t>(1u << c);
            const uint16_t mask = keypad::maskFromMatrix(cols);
            CHECK(mask == (1u << expectedKey[r][c]));
            seen |= mask;
        }
    }
    CHECK(seen == 0xFFFF);  // all 16 keys reachable, none twice

    uint8_t none[4] = {0, 0, 0, 0};
    CHECK(keypad::maskFromMatrix(none) == 0);
    uint8_t two[4] = {0x01, 0x00, 0x00, 0x02};  // keys 1 and 0
    CHECK(keypad::maskFromMatrix(two) == ((1u << 1) | (1u << 0)));
    uint8_t high[4] = {0xF0, 0xF0, 0xF0, 0xF0};  // bits above the 4 columns are ignored
    CHECK(keypad::maskFromMatrix(high) == 0);
}

static void testDebounce() {
    keypad::Debouncer d;
    CHECK(d.update(0) == 0);
    CHECK(d.update(0x0010) == 0);          // first sighting: not yet
    CHECK(d.update(0x0010) == 0x0010);     // second in a row: pressed
    CHECK(d.update(0x0010) == 0x0010);
    CHECK(d.update(0) == 0x0010);          // one missing scan: still held
    CHECK(d.update(0x0010) == 0x0010);     // bounce ignored
    CHECK(d.update(0) == 0x0010);
    CHECK(d.update(0) == 0);               // two in a row: released

    // A one-scan glitch never registers.
    keypad::Debouncer g;
    g.update(0);
    CHECK(g.update(0x0001) == 0);
    CHECK(g.update(0) == 0);
    CHECK(g.update(0) == 0);

    // Keys are independent of each other.
    keypad::Debouncer m;
    m.update(0x0003);
    CHECK(m.update(0x0003) == 0x0003);
    CHECK(m.update(0x0001) == 0x0003);     // key 1 released once, still held
    CHECK(m.update(0x0001) == 0x0001);     // now key 1 is gone, key 0 stays
}

int main() {
    testPackSinglePixel();
    testPackCornersAndPages();
    testPackFullAndBlank();
    testPackOtherSizes();
    testChangedPages();
    testMatrixLayout();
    testDebounce();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
