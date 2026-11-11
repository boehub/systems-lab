# Firmware

The emulator core running on a microcontroller (Raspberry Pi Pico or ESP32) with a small OLED and buttons, so compiled programs run on real hardware.

**Status:** In progress. The portable half is done and runs on a desktop. Nothing has run on a physical board yet.

## What works today

The run loop (`src/main.cpp`) is finished and doesn't know what it's running on. It only talks to the outside world through a six-function interface in `src/platform.h`: set up, read the clock, sleep, show a frame, read the buttons, and say whether to keep running.

`host/host_platform.cpp` implements that interface for a desktop, drawing to the terminal. It lets the full pipeline be tested with no hardware:

```
make run
```

That assembles `../compiler/examples/zero.asm`, turns it into a C array (`build/rom.h`), compiles it with the emulator core and the run loop, and runs it for about two seconds.

## Porting to a board

Porting comes down to writing one more file that implements `platform.h`:

| Function | On the host | On the board |
|----------|-------------|--------------|
| `drawFrame` | prints `#` and `.` | send pixels to the OLED (2x2 pixels per CHIP-8 pixel on a 128x64 screen) |
| `readKeys` | returns 0 | read the GPIO buttons |
| `millisNow` / `sleepMs` | desktop clock | hardware timer |
| `init` | starts the clock | set up I2C, GPIO, timer |
| `keepRunning` | stops after ~2 s | always true |

The emulator core is a good fit for a small chip: the whole machine state is about 6 KB, and `make check-core` in `../cpu-emulator` guarantees it has no OS or I/O dependencies.

## Still to decide

- Pico vs. ESP32, and which toolchain (Pico SDK or Arduino-style)
- How to load programs: embedded in flash at build time (works now) or loaded over USB/serial
- How many buttons to use for the 16-key pad

## Layout

```
firmware/
├── src/
│   ├── main.cpp            portable run loop
│   └── platform.h          hardware interface
├── host/
│   └── host_platform.cpp   desktop implementation (terminal)
├── docs/
│   ├── parts-list.md       candidate parts
│   └── wiring.md           placeholder for the wiring diagram
├── Makefile
└── README.md
```
