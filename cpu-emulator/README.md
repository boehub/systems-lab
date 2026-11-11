# CPU Emulator

An emulator for the CHIP-8 virtual machine, written from scratch in C++17.

**Status:** In progress. It can run programs that draw sprites. Input, real-time timing, and most opcodes are still to do.

## Build and run

```
make
./chip8 roms/zero.ch8
```

This runs up to 1000 instructions, then prints the 64x32 display as `#` and `.`. Programs can be written in assembly and built with `../compiler/chip8asm.py`.

## Implemented opcodes

| Opcode | Meaning |
|--------|---------|
| `00E0` | Clear the screen |
| `1NNN` | Jump to `NNN` |
| `6XNN` | `VX = NN` |
| `7XNN` | `VX += NN` |
| `ANNN` | `I = NNN` |
| `DXYN` | Draw an N-row sprite at (`VX`, `VY`); `VF` is set on collision |

An opcode that isn't implemented stops the machine and prints which one it was.

## Next up

- Subroutines: `2NNN` (call) and `00EE` (return)
- Skips: `3XNN`, `4XNN`, `5XY0`, `9XY0`
- The `8XY_` arithmetic and logic family
- Timers, the font sprites, keypad input, and the rest of the `F___` opcodes

## Design: a core that ports

`src/chip8.h` and `src/chip8.cpp` are the whole machine and do no I/O: no files, no stdio, no exceptions. ROMs go in as a byte buffer, and the display is just an array. `src/main.cpp` is the only part that touches the desktop (reading a ROM file, printing the screen).

That split is what lets the same core run on a microcontroller (see `../firmware`). `make check-core` enforces it by compiling the core with `-fno-exceptions -fno-rtti` and failing if an I/O header sneaks in.

## Layout

```
cpu-emulator/
├── src/
│   ├── chip8.h / chip8.cpp   the machine (portable)
│   └── main.cpp              desktop front end
├── roms/                     test ROMs
├── Makefile
└── README.md
```
