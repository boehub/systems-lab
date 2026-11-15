#pragma once

// Pin assignments for the Raspberry Pi Pico. See ../docs/wiring.md for the
// same table as a wiring guide. GP numbers are the chip's GPIO names, not the
// physical pin numbers.

// SSD1306 OLED on I2C0
#define PIN_OLED_SDA 4    // physical pin 6
#define PIN_OLED_SCL 5    // physical pin 7
#define OLED_I2C_ADDRESS 0x3C
#define OLED_I2C_HZ 1000000  // 1 MHz keeps a full-screen redraw under one 60 Hz frame

// 4x4 button matrix: the Pico drives the rows and reads the columns.
#define PIN_ROW_0 10      // physical pin 14
#define PIN_ROW_1 11      // physical pin 15
#define PIN_ROW_2 12      // physical pin 16
#define PIN_ROW_3 13      // physical pin 17
#define PIN_COL_0 14      // physical pin 19
#define PIN_COL_1 15      // physical pin 20
#define PIN_COL_2 16      // physical pin 21
#define PIN_COL_3 17      // physical pin 22

// Buzzer (an "active" buzzer, which beeps when given power)
#define PIN_BUZZER 18     // physical pin 24
