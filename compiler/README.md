# Assembler / Compiler

A toolchain for the CHIP-8 machine in `../cpu-emulator`. It starts with an assembler; a small programming language that compiles down to it comes next.

**Status:** In progress. The assembler is done and tested. The language is still at the design stage.

## Assembler

`chip8asm.py` is a two-pass assembler (Python 3, standard library only). It supports:

- The full standard CHIP-8 instruction set (`CLS`, `JP`, `CALL`, `SE`, `LD`, `ADD`, `DRW`, and so on)
- Labels, forward and backward references
- `DB` and `DW` data directives
- Hex (`0x1F`), binary (`0b101`), and decimal numbers
- Error messages with line numbers (`line 3: undefined label 'loop'`)
- Output as a raw `.ch8` ROM, or as a C array for embedding in firmware

Example (`examples/zero.asm`):

```
start:  LD   I, glyph      ; I -> sprite data
        DRW  V0, V1, 5     ; draw 5 rows at (0, 0)
done:   JP   done          ; spin here

glyph:  DB   0xF0, 0x90, 0x90, 0x90, 0xF0
```

### Usage

```
python3 chip8asm.py examples/zero.asm -o zero.ch8                 # raw ROM
python3 chip8asm.py examples/zero.asm --format c -o rom.h         # C array
../cpu-emulator/chip8 zero.ch8                                    # run it
```

Two bigger programs run in the emulator as they stand: `examples/zeros.asm` (a subroutine and a loop that draw three glyphs in a row) and `examples/sum.asm` (adds up 1 to 10 and draws the answer, 55, using the built-in font).

### Tests

```
python3 -m unittest discover -s tests
```

The tests check the encoding of every instruction, labels, directives, each kind of error, and that `zero.asm` assembles to exactly the same bytes as the hand-made ROM in `../cpu-emulator/roms/`.

The emulator runs every instruction the assembler can produce except `SYS` (`0NNN`), which it deliberately stops on.

## Language (planned)

A small language with variables, math, and loops that compiles to this assembly. `docs/grammar.md` has the draft grammar. CHIP-8 has no text output, so `print` will draw digits using the built-in font sprites; `examples/sum.asm` shows the pattern by hand (compute, split into decimal digits, draw).

## Custom instruction set (maybe later)

`docs/instruction-set.md` is an early draft of an instruction set of my own. The assembler targets CHIP-8 for now so that everything runs end to end today; a custom machine is a possible later step.

## Layout

```
compiler/
├── chip8asm.py      the assembler
├── tests/           unit tests
├── examples/        sample programs
├── docs/            grammar and instruction-set drafts
└── README.md
```
