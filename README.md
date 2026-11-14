# systems-lab

[![CI](https://github.com/boehub/systems-lab/actions/workflows/ci.yml/badge.svg)](https://github.com/boehub/systems-lab/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

A collection of systems-level projects built from scratch: CPU emulators, compilers, and embedded hardware.

## About

**systems-lab** is my workspace for exploring how computers work from the ground up. It holds projects across the stack, from emulators and toolchains to firmware running on real hardware. Each project lives in its own folder with its own README, build instructions, and notes on what I learned. New experiments get added over time.

![The emulator running a small assembly program in the terminal: a ball moved with the keyboard](docs/demo.gif)

*`compiler/examples/move.asm`, assembled by `chip8asm.py` and run in the emulator.*

## Projects

| Project | Description | Status |
|---------|-------------|--------|
| [CHIP-8 Emulator](cpu-emulator/) | C++ emulator for the CHIP-8 virtual machine. The core has no I/O, so it ports to microcontrollers | Playable: full CPU, real-time terminal front end with keyboard, checked against community test ROMs |
| [Assembler / Compiler](compiler/) | Python assembler for CHIP-8, plus Nibble, a small language (variables, loops, `if`, printing numbers) that compiles to it | Working: tested end to end against the emulator, including on random programs |
| [Firmware](firmware/) | Portable run loop and hardware interface for running the emulator on a microcontroller | In progress (runs on desktop, board port next) |

More coming as the lab grows.

## How the pieces fit

```
  program.nib        (Nibble: x = 2 + 3 * 4  print(x))
      |
      |  compiler/nibc.py   (lexer, parser, checker, code generator)
      v
  program.asm        (or write assembly by hand)
      |
      |  compiler/chip8asm.py
      v
  ROM bytes ---------------> cpu-emulator   (desktop, terminal output)
      |
      |  as a C array
      v
  firmware run loop ---> platform layer ---> terminal today, OLED + buttons later
        (uses the same emulator core)
```

## Try it

From the repo root (needs `g++`, `make`, and Python 3):

```
# Run the assembler and compiler tests
cd compiler && python3 -m unittest discover -s tests && cd ..

# Compile a Nibble program and run it
python3 compiler/nibc.py compiler/examples/fib.nib -o fib.ch8
make -C cpu-emulator && cpu-emulator/chip8 fib.ch8

# Assemble a program and run it in the emulator
python3 compiler/chip8asm.py compiler/examples/zero.asm -o zero.ch8
make -C cpu-emulator && cpu-emulator/chip8 --headless zero.ch8

# Run the full pipeline through the firmware loop
make -C firmware run
```

## Repo Layout

```
systems-lab/
├── cpu-emulator/   emulator core (src/), test ROMs (roms/)
├── compiler/       assembler (chip8asm.py), Nibble compiler (nibc.py), examples, tests, docs
├── firmware/       portable run loop (src/), desktop platform (host/)
├── docs/           demo recording
├── LICENSE         MIT
└── README.md
```

## About Me

I'm Ryan, a student at Old Dominion University. I'm building this repo to learn how things work under the hood.

## License

[MIT](LICENSE). The community test ROMs used by `make conformance` are downloaded at test time and are not part of this repository.
