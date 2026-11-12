# CPU Emulator

An emulator for the CHIP-8 virtual machine, written from scratch in C++17.

**Status:** In progress. It runs programs with subroutines, loops, and conditionals, and draws sprites. Arithmetic opcodes, input, and real-time timing are still to do.

## Build and run

```
make
./chip8 roms/zero.ch8
make test        # unit tests for the core
```

This runs up to 1000 instructions, then prints the 64x32 display as `#` and `.`. Programs can be written in assembly and built with `../compiler/chip8asm.py`.

## Implemented opcodes

| Opcode | Meaning |
|--------|---------|
| `00E0` | Clear the screen |
| `00EE` | Return from a subroutine |
| `1NNN` | Jump to `NNN` |
| `2NNN` | Call the subroutine at `NNN` (16 levels deep) |
| `3XNN` | Skip the next instruction if `VX == NN` |
| `4XNN` | Skip the next instruction if `VX != NN` |
| `5XY0` | Skip the next instruction if `VX == VY` |
| `6XNN` | `VX = NN` |
| `7XNN` | `VX += NN` |
| `9XY0` | Skip the next instruction if `VX != VY` |
| `ANNN` | `I = NNN` |
| `DXYN` | Draw an N-row sprite at (`VX`, `VY`); `VF` is set on collision |

The machine stops, and the front end prints which opcode and where, when it hits an opcode that isn't implemented, a return with nothing on the stack, or a 17th nested call.

## Next up

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
├── tests/test_core.cpp       unit tests (87 checks)
├── roms/                     test ROMs
├── Makefile
└── README.md
```
