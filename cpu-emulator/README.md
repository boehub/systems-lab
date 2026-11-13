# CPU Emulator

An emulator for the CHIP-8 virtual machine, written from scratch in C++17.

**Status:** The CPU is complete: all 34 CHIP-8 instructions plus the built-in font. The desktop front end is still basic (no real-time speed, no keyboard), so that's the next job.

## Build and run

```
make
./chip8 roms/zero.ch8
make test        # unit tests for the core
```

This runs up to 1000 instructions, then prints the 64x32 display as `#` and `.`. Programs can be written in assembly and built with `../compiler/chip8asm.py`; `../compiler/examples/sum.asm` adds up 1 to 10 and draws the answer.

## Instructions

| Opcode | Meaning |
|--------|---------|
| `00E0` | Clear the screen |
| `00EE` | Return from a subroutine |
| `1NNN` | Jump to `NNN` |
| `2NNN` | Call the subroutine at `NNN` (16 levels deep) |
| `3XNN` / `4XNN` | Skip the next instruction if `VX == NN` / `VX != NN` |
| `5XY0` / `9XY0` | Skip the next instruction if `VX == VY` / `VX != VY` |
| `6XNN` / `7XNN` | `VX = NN` / `VX += NN` |
| `8XY0` | `VX = VY` |
| `8XY1` `8XY2` `8XY3` | `VX` OR / AND / XOR `VY` |
| `8XY4` | `VX += VY`; `VF` = carry |
| `8XY5` / `8XY7` | `VX -= VY` / `VX = VY - VX`; `VF` = 1 if there was no borrow |
| `8XY6` / `8XYE` | Shift `VX` right / left; `VF` = the bit shifted out |
| `ANNN` | `I = NNN` |
| `BNNN` | Jump to `NNN + V0` |
| `CXNN` | `VX` = random byte AND `NN` |
| `DXYN` | Draw an N-row sprite at (`VX`, `VY`); `VF` is set on collision |
| `EX9E` / `EXA1` | Skip if key `VX` is held / not held |
| `FX07` / `FX15` / `FX18` | `VX` = delay timer / delay timer = `VX` / sound timer = `VX` |
| `FX0A` | Wait for a key press and store it in `VX` |
| `FX1E` | `I += VX` |
| `FX29` | `I` = address of the font glyph for digit `VX` |
| `FX33` | Store `VX` as three decimal digits at `I`, `I+1`, `I+2` |
| `FX55` / `FX65` | Store / load `V0`..`VX` at memory starting at `I` |

`0NNN` (call native machine code) is deliberately not supported. Treating it as a no-op would make a program that runs off into empty memory spin silently instead of stopping with an error.

The machine stops, and the front end prints which opcode and where, on an unimplemented opcode, a return with nothing on the stack, a 17th nested call, or a memory access past the end of RAM.

### Quirks

A few instructions behave differently on different historical interpreters. They're flags on `Chip8::quirks`, defaulting to what most ROMs written since the 1990s expect:

| Flag | Default | When set |
|------|---------|----------|
| `shiftUsesVy` | shift `VX` in place | `8XY6`/`8XYE` shift `VY` into `VX` (original COSMAC VIP) |
| `jumpUsesVx` | `BNNN` jumps to `NNN + V0` | jumps to `NNN + VX` (CHIP-48 / SUPER-CHIP) |
| `loadStoreIncrementsI` | `FX55`/`FX65` leave `I` alone | `I` advances by `X + 1` (original COSMAC VIP) |

## How it's tested

- **Unit tests** (`make test`): 216 checks covering every instruction, carry and borrow flags, `VF` used as an operand, both quirk settings, stack overflow and underflow, and reads and writes at the edges of memory. I also checked the tests by breaking opcodes on purpose and confirming they fail.
- **Community test ROMs:** I ran the core headless against [Timendus's chip8-test-suite](https://github.com/Timendus/chip8-test-suite). The corax+ opcode test shows 22 of 22 checkmarks, the flags test 49 of 49, and both logo ROMs draw correctly. The ROMs aren't bundled in this repo.

## Next up

- A real front end: run at a steady speed, tick the timers at 60 Hz, and read the keyboard
- Sound (the sound timer already counts down)
- The suite's quirks and keypad tests, once there is keyboard input

## Design: a core that ports

`src/chip8.h` and `src/chip8.cpp` are the whole machine and do no I/O: no files, no stdio, no exceptions. ROMs go in as a byte buffer, and the display is just an array. Random numbers come from a small built-in generator (`seedRandom`), so there's no library dependency. `src/main.cpp` is the only part that touches the desktop (reading a ROM file, printing the screen).

That split is what lets the same core run on a microcontroller (see `../firmware`). `make check-core` enforces it by compiling the core with `-fno-exceptions -fno-rtti` and failing if an I/O header sneaks in.

## Layout

```
cpu-emulator/
├── src/
│   ├── chip8.h / chip8.cpp   the machine (portable)
│   └── main.cpp              desktop front end
├── tests/test_core.cpp       unit tests (216 checks)
├── roms/                     test ROMs
├── Makefile
└── README.md
```
