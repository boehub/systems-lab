# systems-lab

A collection of systems-level projects built from scratch: CPU emulators, compilers, and embedded hardware.

## About

**systems-lab** is my workspace for exploring how computers work from the ground up. It holds projects across the stack, from emulators and toolchains to firmware running on real hardware. Each project lives in its own folder with its own README, build instructions, and notes on what I learned. New experiments get added over time.

## Projects

| Project | Description | Status |
|---------|-------------|--------|
| [CHIP-8 Emulator](cpu-emulator/) | C++ emulator for the CHIP-8 virtual machine. The core has no I/O, so it ports to microcontrollers | In progress (12 opcodes: calls, loops, and conditionals work) |
| [Assembler / Compiler](compiler/) | Python assembler for CHIP-8 with labels, data directives, and tests. A small language comes next | In progress (assembler works) |
| [Firmware](firmware/) | Portable run loop and hardware interface for running the emulator on a microcontroller | In progress (runs on desktop, board port next) |

More coming as the lab grows.

## How the pieces fit

```
  program.asm
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
# Run the assembler tests
cd compiler && python3 -m unittest discover -s tests && cd ..

# Assemble a program and run it in the emulator
python3 compiler/chip8asm.py compiler/examples/zero.asm -o zero.ch8
make -C cpu-emulator && cpu-emulator/chip8 zero.ch8

# Run the full pipeline through the firmware loop
make -C firmware run
```

## Repo Layout

```
systems-lab/
├── cpu-emulator/   emulator core (src/), test ROMs (roms/)
├── compiler/       assembler (chip8asm.py), examples, tests, design docs
├── firmware/       portable run loop (src/), desktop platform (host/)
└── README.md
```

## About Me

I'm Ryan, a student at Old Dominion University. I'm building this repo to learn how things work under the hood.
