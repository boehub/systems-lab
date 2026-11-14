# Assembler / Compiler

A toolchain for the CHIP-8 machine in `../cpu-emulator`: an assembler, and a small programming language (Nibble) that compiles to it.

**Status:** Both work and are tested. Programs written in Nibble compile, assemble and run in the emulator.

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
../cpu-emulator/chip8 --headless zero.ch8                                    # run it
```

Three bigger programs run in the emulator as they stand: `examples/move.asm` (a ball you move with the keyboard; keys 2, S, Q, E), `examples/zeros.asm` (a subroutine and a loop that draw three glyphs in a row) and `examples/sum.asm` (adds up 1 to 10 and draws the answer, 55, using the built-in font).

### Tests

```
python3 -m unittest discover -s tests
```

The tests check the encoding of every instruction, labels, directives, each kind of error, and that `zero.asm` assembles to exactly the same bytes as the hand-made ROM in `../cpu-emulator/roms/`.

The emulator runs every instruction the assembler can produce except `SYS` (`0NNN`), which it deliberately stops on.

## Nibble, a small language

```
// primes.nib (excerpt)
n = 2
while n < 72 {
    prime = 1
    d = 2
    while d * d <= n and prime {
        m = d
        while m < n { m = m + d }
        if m == n { prime = 0 }
        d = d + 1
    }
    if prime { print(n) }
    n = n + 1
}
```

Nibble has variables, arithmetic, comparisons, `and`/`or`/`not`, `if`/`else`, `while`, and built-ins to draw numbers and read the keypad. Everything is one byte and wraps around, like a CHIP-8 register. See `docs/language.md` for the full reference and grammar.

```
python3 nibc.py examples/primes.nib -o primes.ch8     # compile to a ROM
python3 nibc.py examples/primes.nib -S                # show the generated assembly
../cpu-emulator/chip8 primes.ch8                      # run it
```

CHIP-8 has no text output, so `print` draws each digit with the built-in font sprites. Examples: `hello.nib`, `fib.nib`, `primes.nib`, and `counter.nib` (steer a number with the keypad).

The compiler is split into a lexer, parser, checker and code generator (`nibble/`). Errors come with the line, the source text and a caret:

```
bad.nib: error: line 2: 'i' is used before it is given a value
    while i < 10 {
          ^
```

### How it's tested

`tests/test_nibble.py` has unit tests for each stage, and end-to-end tests that compile a program, run it in the real emulator, and read the digits off the screen. A reference interpreter (`tests/nibble_ref.py`) gives the expected answer for 120 random programs (loops, branches, every operator), and the two must agree exactly. I also broke the code generator on purpose in five ways (a flipped comparison, a skipped register restore, and so on) and checked the tests catch each. The end-to-end tests need `../cpu-emulator/chip8` built; they skip if it isn't, and CI requires it.

## Custom instruction set (maybe later)

`docs/instruction-set.md` is an early draft of an instruction set of my own, and `examples/hello.asm` a sketch for it. Nibble targets CHIP-8 so that everything runs end to end today; a custom machine is a possible later step.

## Layout

```
compiler/
├── chip8asm.py      the assembler
├── nibc.py          the Nibble compiler (command line)
├── nibble/          lexer, parser, checker, code generator
├── tests/           assembler and compiler tests
├── examples/        sample programs (.asm and .nib)
├── docs/            language reference, instruction-set draft
└── README.md
```
