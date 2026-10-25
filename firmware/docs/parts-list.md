# Parts List (Planning)

Candidate parts for running the emulator on real hardware. Nothing is bought or final yet.

## Microcontroller (pick one)

- Raspberry Pi Pico (RP2040)
- ESP32 dev board

## Display

- SSD1306 128x64 I2C OLED. CHIP-8's 64x32 screen fits when each pixel is drawn as 2x2.

## Input

- Tactile push buttons. The CHIP-8 keypad has 16 keys; start with 4–6 buttons and expand later.

## Extras

- Piezo buzzer for the sound timer
- Breadboard and jumper wires

## Open questions

- Pico vs. ESP32: which has the easier toolchain for C++?
- Where to store ROMs: flash on the board, or load over USB/serial?
