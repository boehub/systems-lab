# Nibble language reference

Nibble is a small language that compiles to CHIP-8. A program is a list of statements. There are variables, arithmetic, `if`/`else`, `while`, and a few built-ins for drawing numbers and reading the keypad.

```
// Prints 14
print(2 + 3 * 4)
```

## Numbers

Every value is one byte, 0 to 255, because that's what a CHIP-8 register holds. Arithmetic wraps around: `200 + 100` is `44` and `3 - 5` is `254`. Literals can be decimal, `0x1F` or `0b101`; a literal above 255 is an error. There are no negative numbers, no fractions and no booleans: comparisons give `0` or `1`, and in a condition any nonzero value counts as true.

## Variables

```
x = 5
x = x + 1
```

A variable exists from its first assignment and must be given a value before it is read (an error otherwise, with the line number). Names are letters, digits and `_`, not starting with a digit. There is one scope. A program can have up to 12 variables.

## Expressions

From loosest to tightest binding:

| Operators | Meaning |
|-----------|---------|
| `or` | logical or (stops early; result is 0 or 1) |
| `and` | logical and (stops early; result is 0 or 1) |
| `not` | logical not, applies to a whole comparison: `not a == b` |
| `==` `!=` `<` `<=` `>` `>=` | unsigned comparison, gives 0 or 1; can't be chained (`a < b < c` is an error) |
| `\|` | bitwise or |
| `^` | bitwise xor |
| `&` | bitwise and |
| `+` `-` | add, subtract (wrap around) |
| `*` | multiply (wraps; done by repeated addition, so it takes time proportional to the right operand) |

The bitwise operators bind tighter than comparisons, as in Python (not as in C), so `x & 1 == 0` means `(x & 1) == 0`. Parentheses group. `not` has to start an operand, so write `a + (not b)`.

Parts of an expression that don't depend on running the program are computed by the compiler: `2 + 3 * 4` compiles to a single load of `14`.

## Statements

```
x = expression

if condition { ... }
if condition { ... } else { ... }
if condition { ... } else if condition { ... } else { ... }

while condition { ... }
```

Braces are required. Semicolons are optional. Comments start with `//`.

## Built-ins

| Call | What it does |
|------|--------------|
| `print(e)` | draw the value of `e` in decimal (no leading zeros) at the cursor, then leave a space |
| `newline()` | move the cursor to the start of the next row of text |
| `at(x, y)` | move the cursor to pixel (x, y) |
| `clear()` | clear the screen and put the cursor top-left |
| `wait(n)` | pause for `n` sixtieths of a second |
| `key()` | wait for a key to be pressed and released; its value is the key (0 to 15) |
| `pressed(k)` | 1 if keypad key `k` is held down right now, otherwise 0 |
| `rand()` | a random number, 0 to 255 |

CHIP-8 has no text mode, so `print` draws each digit with the built-in font: 4 pixels wide, 5 tall, at 5 pixels per character, and rows of text are 6 pixels apart. The screen is 64 by 32 pixels, so it fits about 12 digits across and 5 rows. Drawing is XOR, so printing over text that's already there makes a mess; use `clear()` or `at()` to start fresh. The keypad keys are mapped to the keyboard by the emulator (see `../cpu-emulator/README.md`).

## Grammar

```
program     = { statement } ;
statement   = assignment | if | while | call ;
assignment  = identifier "=" expression ;
if          = "if" expression block [ "else" ( if | block ) ] ;
while       = "while" expression block ;
call        = ( "print" | "wait" ) "(" expression ")"
            | "at" "(" expression "," expression ")"
            | ( "newline" | "clear" ) "(" ")" ;
block       = "{" { statement } "}" ;

expression  = or ;
or          = and { "or" and } ;
and         = not { "and" not } ;
not         = "not" not | comparison ;
comparison  = bit_or [ ( "==" | "!=" | "<" | "<=" | ">" | ">=" ) bit_or ] ;
bit_or      = bit_xor { "|" bit_xor } ;
bit_xor     = bit_and { "^" bit_and } ;
bit_and     = sum { "&" sum } ;
sum         = product { ( "+" | "-" ) product } ;
product     = primary { "*" primary } ;
primary     = number | identifier | "(" expression ")"
            | "key" "(" ")" | "rand" "(" ")" | "pressed" "(" expression ")" ;

number      = decimal | "0x" hexdigits | "0b" bits ;
identifier  = ( letter | "_" ) { letter | digit | "_" } ;   (not a keyword or built-in)
```

## How it compiles

`nibc.py` runs four stages, each in its own file in `nibble/`:

1. `lexer.py` turns text into tokens, with line and column for error messages.
2. `parser.py` is a recursive-descent parser that builds the syntax tree (`tree.py`).
3. `check.py` finds the variables, rejects reads before assignment, and folds constants.
4. `codegen.py` writes CHIP-8 assembly, which `chip8asm.py` turns into a ROM.

Registers: `V0`..`VB` hold your variables first, then temporaries used while evaluating expressions. `VC` and `VD` are the text cursor, `VE` is scratch for `print`, and `VF` is the flag that CHIP-8 arithmetic overwrites (comparisons read it right after a subtraction). If an expression needs more registers than are free, you get a compile error rather than wrong code. `print` is a subroutine added only when used; it borrows `V0`-`V2` for the digits and puts them back.

`nibc.py prog.nib -S` prints the generated assembly.

## Not yet

- Functions
- Arrays and sprites (so far everything drawn is a number)
- Division and remainder (see `examples/primes.nib` for how to get by without)
