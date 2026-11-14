# CPU Emulator

An emulator for the CHIP-8 virtual machine, written from scratch in C++17.

**Status:** Complete and playable. All 34 CHIP-8 instructions plus the font, a real-time terminal front end (steady speed, 60 Hz timers, keyboard), and checked against the community test ROMs.

## Build and run

```
make
./chip8 game.ch8                  # play it in the terminal (Esc quits)
./chip8 --headless roms/zero.ch8  # no terminal: run, then print the screen as # and .
```

In a terminal the picture is drawn with half-block characters (64x16 cells, so the terminal needs to be at least 64 columns by 17 rows). The program runs at a steady `--speed` instructions per second, and the delay and sound timers tick at 60 Hz in real time no matter the speed. If stdin or stdout isn't a terminal it runs headless automatically.

Programs can be written in assembly and built with `../compiler/chip8asm.py`; `../compiler/examples/sum.asm` adds up 1 to 10 and draws the answer.

### Keyboard

The CHIP-8 keypad is a 4x4 grid, mapped onto the left of a QWERTY keyboard:

```
CHIP-8 keypad     your keyboard
1 2 3 C           1 2 3 4
4 5 6 D           Q W E R
7 8 9 E           A S D F
A 0 B F           Z X C V
```

Terminals report key presses but not releases, so a key counts as held for 150 ms after the last press (`--hold` changes this). Holding a key down works through auto-repeat.

### Options

| Option | Meaning |
|--------|---------|
| `--speed N` | Instructions per second (default 700) |
| `--hold N` | How long a key stays down after a press, in ms (default 150) |
| `--beep` | Ring the terminal bell when the sound timer starts |
| `--seed N` | Seed the random number generator (default: from the clock; headless uses a fixed seed) |
| `--headless` | Don't use the terminal; run `--cycles N` instructions (default 1000) and print the screen |
| `--quirk-shift`, `--quirk-jump`, `--quirk-loadstore`, `--quirk-vfreset`, `--quirk-displaywait` | Turn on one quirk (see below) |
| `--vip` | Original COSMAC VIP behaviour: shift, loadstore, vfreset and displaywait |

Exit status: 0 normally, 1 if the machine stopped on an error, 2 for a usage error.

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
| `logicResetsVf` | `8XY1`/`8XY2`/`8XY3` leave `VF` alone | they set `VF` to 0 (original COSMAC VIP) |
| `displayWait` | `DXYN` draws immediately | at most one sprite per 60 Hz frame; a second `DXYN` waits for the next tick (original COSMAC VIP) |

## How it's tested

- **Unit tests** (`make test`): 252 checks on the core (every instruction, carry and borrow flags, `VF` used as an operand, every quirk both ways, stack overflow and underflow, reads and writes at the edges of memory) and 123 on the front-end logic (options, key layout, key hold window, exact instruction pacing, rendering). I also checked the tests by breaking the code on purpose and confirming they fail.
- **End-to-end tests** (`make test-interactive`): 14 tests that run the real program inside a pseudo-terminal. They press keys, check the timer takes a real second to count 60 ticks, check Esc and Ctrl-C restore the terminal, and cover the headless mode and error messages.
- **Community test ROMs** (`make conformance`): runs [Timendus's chip8-test-suite](https://github.com/Timendus/chip8-test-suite) and counts the checkmarks it draws. Currently 22 of 22 on the corax+ opcode test, 49 of 49 on the flags test, and 6 of 6 on the quirks test (with `--vip`, choosing the CHIP-8 platform). The ROMs are downloaded on first use, checked against a SHA-256 fingerprint, and cached in a git-ignored folder; they aren't part of this repo.

## Next up

- Sound: only a bell for now
- The suite's keypad test, and a SUPER-CHIP mode

## Design: a core that ports

`src/chip8.h` and `src/chip8.cpp` are the whole machine and do no I/O: no files, no stdio, no exceptions. ROMs go in as a byte buffer, and the display is just an array. Random numbers come from a small built-in generator (`seedRandom`), so there's no library dependency. Everything that touches the desktop lives outside it: `src/main.cpp` (the loop and ROM loading), `src/terminal.cpp` (raw mode and drawing) and `src/frontend.cpp` (option parsing, keyboard mapping, pacing and rendering, with no OS calls so it's unit-testable).

That split is what lets the same core run on a microcontroller (see `../firmware`). `make check-core` enforces it by compiling the core with `-fno-exceptions -fno-rtti` and failing if an I/O header sneaks in.

## Layout

```
cpu-emulator/
├── src/
│   ├── chip8.h / chip8.cpp   the machine (portable)
│   ├── frontend.h / .cpp     options, keys, pacing, rendering (no OS calls)
│   ├── terminal.h / .cpp     raw-mode terminal
│   └── main.cpp              the real-time loop
├── tests/
│   ├── test_core.cpp         core unit tests
│   ├── test_frontend.cpp     front-end unit tests
│   ├── test_interactive.py   end-to-end tests through a pseudo-terminal
│   └── conformance.py        community test ROMs
├── roms/                     test ROMs
├── Makefile
└── README.md
```
