# Wiring

Pin assignments for the Raspberry Pi Pico build. They are all in one file, `pico/pins.h`, if you want different ones. "GP" numbers are the chip's GPIO names; the physical pin number is what you count on the board.

| Signal | Pico pin | Connects to |
|--------|----------|-------------|
| OLED SDA | GP4 (pin 6) | OLED SDA |
| OLED SCL | GP5 (pin 7) | OLED SCL |
| OLED VCC | 3V3 OUT (pin 36) | OLED VCC |
| OLED GND | GND (pin 8) | OLED GND |
| Keypad row 0 | GP10 (pin 14) | keypad row 0 (top row: 1 2 3 C) |
| Keypad row 1 | GP11 (pin 15) | keypad row 1 (4 5 6 D) |
| Keypad row 2 | GP12 (pin 16) | keypad row 2 (7 8 9 E) |
| Keypad row 3 | GP13 (pin 17) | keypad row 3 (A 0 B F) |
| Keypad column 0 | GP14 (pin 19) | keypad column 0 (left) |
| Keypad column 1 | GP15 (pin 20) | keypad column 1 |
| Keypad column 2 | GP16 (pin 21) | keypad column 2 |
| Keypad column 3 | GP17 (pin 22) | keypad column 3 (right) |
| Buzzer drive | GP18 (pin 24) | 1 kΩ resistor to the transistor's base |

## Keypad

A 4x4 membrane keypad has 8 pins in a row, normally four row wires then four column wires, but not always in that order. If a key press shows up as the wrong key, swap the wires; if a whole row or column is wrong, the order is reversed. A multimeter in continuity mode on a pressed key tells you which two pins it joins.

The Pico drives one row high at a time and reads the four columns (which are pulled low), so a pressed button shows up as a high column. Without a diode per button, pressing three keys at once can make a fourth look pressed ("ghosting"); that is normal for a matrix and rarely matters for CHIP-8 games.

Key layout, as the program sees it (this is the standard CHIP-8 pad):

```
1 2 3 C
4 5 6 D
7 8 9 E
A 0 B F
```

## Buzzer (optional)

A buzzer draws more current than a Pico pin should supply, so switch it with a transistor:

```
GP18 --[ 1 kΩ ]-- base
                  NPN (2N2222): collector -- buzzer (-)
                                emitter   -- GND
buzzer (+) -- 3V3
```

If you skip it, nothing else changes; the firmware just drives an unconnected pin.

## Checklist before powering up

- OLED VCC is on 3V3, not VBUS (5 V)
- SDA and SCL are not swapped
- Every wire ends up on a different pin: check rows against columns
