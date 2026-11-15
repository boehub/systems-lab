#include "menu.h"

#include <cstring>

#include "text.h"

namespace {

constexpr int kKeyUp = 2;
constexpr int kKeyDown = 8;
constexpr int kKeySelect = 5;

constexpr int kRuleY = 8;     // line under the title
constexpr int kRowsTop = 10;  // first entry row
constexpr int kRowHeight = 9;
constexpr int kNameChars = 19;

// "n/total" without printf: the standard formatting routines would add about
// 13 KB of flash to the firmware for this one line.
void formatPosition(char* out, int n, int total) {
    auto put = [&out](int value) {
        char digits[12];
        int count = 0;
        do {
            digits[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value > 0);
        while (count > 0) {
            *out++ = digits[--count];
        }
    };
    put(n);
    *out++ = '/';
    put(total);
    *out = '\0';
}

}  // namespace

void Menu::open(uint16_t heldKeys) { previous_ = heldKeys; }

int Menu::update(uint16_t keys) {
    const uint16_t pressed = keys & static_cast<uint16_t>(~previous_);
    previous_ = keys;
    if (count_ <= 0) {
        return -1;
    }

    if (pressed & (1u << kKeyUp)) {
        cursor_ = (cursor_ + count_ - 1) % count_;
    }
    if (pressed & (1u << kKeyDown)) {
        cursor_ = (cursor_ + 1) % count_;
    }

    // Keep the highlighted entry on screen.
    if (cursor_ < top_) {
        top_ = cursor_;
    }
    if (cursor_ >= top_ + kVisible) {
        top_ = cursor_ - kVisible + 1;
    }

    return (pressed & (1u << kKeySelect)) ? cursor_ : -1;
}

void Menu::render(uint8_t* pixels) const {
    std::memset(pixels, 0, kWidth * kHeight);

    text::draw(pixels, kWidth, kHeight, 2, 1, "PROGRAMS");
    char where[24];  // two ints and a slash: at most 23 characters
    formatPosition(where, count_ > 0 ? cursor_ + 1 : 0, count_);
    text::draw(pixels, kWidth, kHeight, kWidth - 2 - text::width(where), 1, where);
    text::fillRect(pixels, kWidth, kHeight, 0, kRuleY, kWidth, 1, 1);

    for (int row = 0; row < kVisible; ++row) {
        const int index = top_ + row;
        if (index >= count_) {
            break;
        }
        const int y = kRowsTop + row * kRowHeight;
        const bool selected = index == cursor_;
        if (selected) {
            text::fillRect(pixels, kWidth, kHeight, 0, y, kWidth, kRowHeight, 1);
        }
        // Selected entries are drawn dark on a lit bar.
        text::draw(pixels, kWidth, kHeight, 3, y + 1, roms_[index].name,
                   selected ? 0 : 1, kNameChars);
    }
}
