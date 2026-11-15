# Firmware

The emulator core running on a Raspberry Pi Pico with a 128x64 OLED and a 4x4 button keypad, so programs compiled with the toolchain in `../compiler` run on real hardware.

**Status:** The Pico firmware builds (and CI builds it), and every part of it that doesn't touch a pin is tested on the desktop. It has not yet run on a physical board: I wrote it against the datasheets and SDK without hardware in hand, so the first power-up is the real test. See "First power-up" below for what to check.

## Layout

```
firmware/
├── src/
│   ├── main.cpp              portable run loop (same code on desktop and Pico)
│   └── platform.h            the interface to the hardware: 7 functions
├── host/
│   └── host_platform.cpp     desktop implementation (terminal)
├── pico/
│   ├── pico_platform.cpp     Pico implementation: I2C OLED, GPIO keypad, buzzer
│   ├── oled_frame.*          pixels -> SSD1306 memory (no SDK, unit-tested)
│   ├── keypad_matrix.*       key matrix decode + debounce (no SDK, unit-tested)
│   ├── pins.h                pin assignments
│   └── CMakeLists.txt        Pico SDK build
├── tests/test_pico_logic.cpp
├── docs/
│   ├── parts-list.md
│   └── wiring.md
└── Makefile                  desktop demo and tests
```

The run loop only talks to hardware through `platform.h`: set up, read the clock, sleep, show a frame, read the keys, set the buzzer, and say whether to keep running. Each target implements those once. The emulator core underneath has no OS or I/O dependencies (`make check-core` in `../cpu-emulator` enforces it), which is what makes the port this small.

## On the desktop

```
make run      # assemble a ROM, build the run loop, run it for about two seconds
make test     # the Pico port's hardware-independent logic
```

## On a Pico

Parts and wiring: `docs/parts-list.md` and `docs/wiring.md`. Then:

```
git clone --branch 2.1.1 https://github.com/raspberrypi/pico-sdk
export PICO_SDK_PATH=$PWD/pico-sdk
# also needs: cmake, gcc-arm-none-eabi (and its newlib), Python 3

cd firmware/pico
mkdir build && cd build
cmake ..                        # embeds compiler/examples/move.asm by default
make
```

That produces `chip8_pico.uf2`. Hold the BOOTSEL button while plugging the Pico in over USB, and copy the file onto the drive that appears. It restarts and runs.

Choosing the program that gets built in:

```
cmake -DROM=../../../compiler/examples/fib.nib ..       # a Nibble program
cmake -DROM=../../../compiler/examples/sum.asm ..       # an assembly program
cmake -DCYCLES_PER_FRAME=20 ..                          # faster (12 = about 720 instructions/s)
cmake -DPICO_BOARD=pico2 ..                             # Pico 2 (it compiles; untested like the rest)
```

The firmware is about 14 KB of flash and 10 KB of RAM (the Pico has 2 MB and 264 KB).

## How it works on the board

- **Display.** Each CHIP-8 pixel is drawn as a 2x2 block on the 128x64 screen. The frame is packed into the OLED's memory layout (8 rows per "page") and only the pages that changed are sent, over I2C at 1 MHz, so a full redraw takes under 10 ms and a static screen costs nothing.
- **Keys.** The Pico drives each keypad row in turn and reads the columns. A key counts once two scans in a row agree (about 17 ms), which filters contact bounce. Unlike the terminal front end, real buttons report release, so no timeout is needed.
- **Timing.** One loop iteration per 60 Hz frame: read keys, run 12 instructions, tick the timers, update the buzzer and screen, sleep the rest of the frame.
- **Memory.** The emulator's 6 KB of state is static, not on the stack (the Pico's default stack is 2 KB).

## First power-up

Things to check, in order, if something looks wrong:

1. **Blank screen.** Check VCC on 3V3, SDA/SCL not swapped, and the I2C address: most modules are `0x3C`, some `0x3D` (`OLED_I2C_ADDRESS` in `pico/pins.h`).
2. **Garbled or shifted picture.** A 1.3" display is often an SH1106 rather than an SSD1306; it needs a 2-column offset.
3. **Keys wrong.** Swap row/column wires as described in `docs/wiring.md`.
4. **Display glitches at 1 MHz.** Some modules only like 400 kHz; change `OLED_I2C_HZ`. Timers then run slightly slow on animated screens.

## Next up

- Run it on a real board and record a photo/video of it working
- Choose among several built-in programs with a menu instead of one baked in at build time
- ESP32 port (it would be one more `platform.h` implementation)
