# CPU Emulator

An emulator for the CHIP-8 virtual machine, written from scratch.

**Status:** Planned

## Goals

- Implement the fetch / decode / execute loop
- Load and run CHIP-8 ROMs
- Render the 64x32 display and handle keypad input
- Keep the emulator core free of OS-specific code so it can be ported to a microcontroller later (see `../firmware`)

## Planned Layout

```
cpu-emulator/
├── src/
├── roms/
└── README.md
```

## Notes

Build instructions and what I learned will go here as the project takes shape.
