#include "oled_frame.h"

#include <cstring>

namespace oled {

void pack(const uint8_t* pixels, int w, int h, uint8_t out[kBufferSize]) {
    std::memset(out, 0, kBufferSize);
    if (w <= 0 || h <= 0) {
        return;
    }
    int scale = kWidth / w < kHeight / h ? kWidth / w : kHeight / h;
    if (scale < 1) {
        scale = 1;  // bigger than the display: crop instead of scaling
    }
    const int offsetX = (kWidth - w * scale) / 2;
    const int offsetY = (kHeight - h * scale) / 2;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!pixels[y * w + x]) {
                continue;
            }
            for (int dy = 0; dy < scale; ++dy) {
                const int sy = offsetY + y * scale + dy;
                if (sy < 0 || sy >= kHeight) {
                    continue;
                }
                for (int dx = 0; dx < scale; ++dx) {
                    const int sx = offsetX + x * scale + dx;
                    if (sx < 0 || sx >= kWidth) {
                        continue;
                    }
                    out[(sy / 8) * kWidth + sx] |= static_cast<uint8_t>(1u << (sy % 8));
                }
            }
        }
    }
}

uint8_t changedPages(const uint8_t* a, const uint8_t* b) {
    uint8_t mask = 0;
    for (int page = 0; page < kPages; ++page) {
        if (std::memcmp(a + page * kWidth, b + page * kWidth, kWidth) != 0) {
            mask |= static_cast<uint8_t>(1u << page);
        }
    }
    return mask;
}

}  // namespace oled
