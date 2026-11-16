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
| [Firmware](firmware/) | The emulator on a Raspberry Pi Pico with an OLED, keypad and buzzer: portable run loop, a program menu, hardware interface, Pico build | Builds for the Pico (also in CI); tests cover everything but the pins; not yet run on a physical board |
| [Shell (`msh`)](shell/) | A Unix shell in C: pipes, redirection, here-documents, `$(...)`, `if` / `while` / `for` / `case`, functions, `( )` and `{ }`, wildcards, and job control (Ctrl-C, Ctrl-Z, `fg`, `bg`) | Working: parser unit tests with a fuzz run, end-to-end tests including a pseudo-terminal, sanitizer runs |
| [GIS reader](gis/) | GeoTIFF and shapefile readers written byte by byte in pure Python (LZW decoder, tiles, dBase attributes), with point-in-polygon and zonal statistics | Working: 78 tests, cross-checked against `tifffile` and `pyshp`, fuzzed with corrupt files |
| [Packet analyzer (`pktscope`)](net/) | Reads pcap and pcapng captures (or captures live on Linux) and decodes Ethernet, VLAN, ARP, IPv4/IPv6, TCP, UDP, ICMP, DNS and HTTP, with a tcpdump-style filter language, checksum checks and conversation summaries | Working: 930+ unit checks, cross-checked against scapy on thousands of random packets, fuzzed, sanitizer runs |

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
  firmware run loop ---> platform layer ---> terminal (desktop) or OLED + keypad (Pico)
        (uses the same emulator core)
```

## Try it

From the repo root (needs `g++`, `make`, and Python 3):

```
# Build the emulator and run all the tests (the compiler's tests run
# programs in the emulator, so it has to be built first)
make -C cpu-emulator && make -C cpu-emulator test test-interactive
(cd compiler && python3 -m unittest discover -s tests)

# Compile a Nibble program and run it
python3 compiler/nibc.py compiler/examples/fib.nib -o fib.ch8
cpu-emulator/chip8 fib.ch8

# Assemble a program and run it in the emulator
python3 compiler/chip8asm.py compiler/examples/zero.asm -o zero.ch8
cpu-emulator/chip8 --headless zero.ch8

# Build and test the shell, then try it
make -C shell test
shell/msh

# Try the GIS reader on the sample data
python3 gis/gis.py zonal gis/examples/dem.tif gis/examples/regions.shp --field NAME

# Read the sample capture with the packet analyzer
make -C net pktscope
net/pktscope net/examples/demo.pcap --summary

# Run the full pipeline through the firmware loop, and see the Pico's program menu
make -C firmware run
make -C firmware menu
```

## Repo Layout

```
systems-lab/
├── cpu-emulator/   emulator core (src/), test ROMs (roms/)
├── compiler/       assembler (chip8asm.py), Nibble compiler (nibc.py), examples, tests, docs
├── firmware/       run loop and program menu (src/), desktop (host/) and Raspberry Pi Pico (pico/) platforms
├── gis/            GeoTIFF + shapefile readers (geo/), command-line tool (gis.py), sample data, tests
├── net/            packet capture analyzer in C (src/), sample capture, tests
├── shell/          Unix shell in C: parser (parse.c), shell and job control (msh.c), tests
├── docs/           demo recording, menu picture
├── LICENSE         MIT
└── README.md
```

## About Me

I'm Ryan, a student at Old Dominion University. I'm building this repo to learn how things work under the hood.

## License

[MIT](LICENSE). The community test ROMs used by `make conformance` are downloaded at test time and are not part of this repository.
