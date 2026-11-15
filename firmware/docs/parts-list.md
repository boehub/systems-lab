# Parts list

What the Raspberry Pi Pico build uses. Nothing here is exotic; the whole thing is a few dollars of parts on a breadboard.

| Part | Notes |
|------|-------|
| Raspberry Pi Pico (RP2040) | A Pico H (headers pre-soldered) is easiest. The Pico 2 also builds (see the README); a Pico W works too, the wireless chip is just unused |
| 0.96" 128x64 I2C OLED, SSD1306 | The common 4-pin module (GND, VCC, SCL, SDA; the pin order varies between sellers, so read the labels). Check the chip: a 1.3" screen is often an SH1106, which needs a small code change |
| 4x4 matrix keypad | The usual membrane keypad with 8 pins (4 rows, 4 columns). Or 16 push buttons wired as a matrix |
| One push button | The MENU button: returns to the program menu |
| Active buzzer, 3.3 V or 5 V (optional) | "Active" means it beeps when powered, no tone generation needed |
| NPN transistor (2N2222 or similar) and a 1 kΩ resistor (optional) | Drives the buzzer; a Pico pin can't supply a buzzer's current directly |
| Breadboard, jumper wires, micro-USB cable | |

## Why these

- **OLED:** CHIP-8's 64x32 screen fits the 128x64 display exactly when each pixel is drawn as 2x2.
- **4x4 keypad:** CHIP-8 has 16 keys in a 4x4 grid, so a matrix keypad is the natural match, and it only uses 8 GPIO pins.
- **Where programs live:** inside the firmware, chosen when you build (`-DROMS=...`), with a menu to pick one on the board. Loading over USB is a possible later step.
