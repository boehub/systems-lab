// Raspberry Pi Pico implementation of platform.h:
//   display  SSD1306 128x64 OLED over I2C (each CHIP-8 pixel drawn as 2x2)
//   input    4x4 button matrix on GPIO
//   sound    active buzzer on a GPIO
//
// The parts that are plain logic live in oled_frame.cpp and keypad_matrix.cpp,
// which are unit-tested on the desktop. This file is only the glue to the SDK.

#include <cstring>

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "keypad_matrix.h"
#include "oled_frame.h"
#include "pico/stdlib.h"
#include "pico/time.h"
#include "pins.h"
#include "platform.h"

namespace {

i2c_inst_t* const kI2c = i2c0;

const uint kRowPins[4] = {PIN_ROW_0, PIN_ROW_1, PIN_ROW_2, PIN_ROW_3};
const uint kColPins[4] = {PIN_COL_0, PIN_COL_1, PIN_COL_2, PIN_COL_3};

// What is currently on the OLED, so only changed pages are sent.
uint8_t shown[oled::kBufferSize];
uint8_t next[oled::kBufferSize];
keypad::Debouncer debouncer;

// A command is sent with control byte 0x00, display data with 0x40.
void oledCommand(uint8_t c) {
    const uint8_t msg[2] = {0x00, c};
    i2c_write_blocking(kI2c, OLED_I2C_ADDRESS, msg, 2, false);
}

void oledInit() {
    static const uint8_t setup[] = {
        0xAE,              // display off
        0xD5, 0x80,        // clock divide
        0xA8, 0x3F,        // 64 rows
        0xD3, 0x00,        // no vertical offset
        0x40,              // start at row 0
        0x8D, 0x14,        // charge pump on
        0x20, 0x00,        // horizontal addressing mode
        0xA1,              // flip horizontally (so x increases to the right)
        0xC8,              // flip vertically
        0xDA, 0x12,        // pin layout for 128x64
        0x81, 0xCF,        // contrast
        0xD9, 0xF1,        // pre-charge
        0xDB, 0x40,        // VCOM level
        0xA4,              // show RAM contents
        0xA6,              // normal (not inverted)
        0xAF,              // display on
    };
    for (uint8_t c : setup) {
        oledCommand(c);
    }
    std::memset(shown, 0, sizeof shown);
}

void oledSendPage(int page, const uint8_t* data) {
    oledCommand(0x21);  // column range: 0..127
    oledCommand(0);
    oledCommand(oled::kWidth - 1);
    oledCommand(0x22);  // page range: just this one
    oledCommand(static_cast<uint8_t>(page));
    oledCommand(static_cast<uint8_t>(page));

    uint8_t msg[1 + oled::kWidth];
    msg[0] = 0x40;
    std::memcpy(msg + 1, data, oled::kWidth);
    i2c_write_blocking(kI2c, OLED_I2C_ADDRESS, msg, sizeof msg, false);
}

}  // namespace

namespace platform {

void init() {
    // Display bus
    i2c_init(kI2c, OLED_I2C_HZ);
    gpio_set_function(PIN_OLED_SDA, GPIO_FUNC_I2C);
    gpio_set_function(PIN_OLED_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(PIN_OLED_SDA);
    gpio_pull_up(PIN_OLED_SCL);
    sleep_ms(100);  // let the display power up
    oledInit();

    // Button matrix: rows are outputs (idle low), columns are inputs that
    // read low unless a pressed button connects them to the driven row.
    for (uint pin : kRowPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, 0);
    }
    for (uint pin : kColPins) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);
    }

    gpio_init(PIN_BUZZER);
    gpio_set_dir(PIN_BUZZER, GPIO_OUT);
    gpio_put(PIN_BUZZER, 0);
}

uint32_t millisNow() { return to_ms_since_boot(get_absolute_time()); }

void sleepMs(uint32_t ms) { sleep_ms(ms); }

void drawFrame(const uint8_t* pixels, int width, int height) {
    oled::pack(pixels, width, height, next);
    const uint8_t changed = oled::changedPages(shown, next);
    for (int page = 0; page < oled::kPages; ++page) {
        if (changed & (1u << page)) {
            oledSendPage(page, next + page * oled::kWidth);
        }
    }
    std::memcpy(shown, next, sizeof shown);
}

uint16_t readKeys() {
    uint8_t columns[4];
    for (int r = 0; r < 4; ++r) {
        gpio_put(kRowPins[r], 1);
        busy_wait_us(10);  // let the line settle
        uint8_t bits = 0;
        for (int c = 0; c < 4; ++c) {
            bits |= static_cast<uint8_t>(gpio_get(kColPins[c]) << c);
        }
        columns[r] = bits;
        gpio_put(kRowPins[r], 0);
    }
    return debouncer.update(keypad::maskFromMatrix(columns));
}

void setBuzzer(bool on) { gpio_put(PIN_BUZZER, on); }

bool keepRunning() { return true; }

}  // namespace platform
