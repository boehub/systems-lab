# Language Grammar (DRAFT)

The first goal is a tiny expression language. The first program to get working:

```
print(2 + 3 * 4)
```

## Grammar (EBNF)

```
program    = { statement } ;
statement  = print_stmt | assign_stmt ;
print_stmt = "print" "(" expression ")" ;
assign_stmt = identifier "=" expression ;

expression = term { ( "+" | "-" ) term } ;
term       = factor { "*" factor } ;
factor     = number | identifier | "(" expression ")" ;

number     = digit { digit } ;
identifier = letter { letter | digit } ;
```

`*` binds tighter than `+` and `-`, so `2 + 3 * 4` is `14`.

On CHIP-8 there is no text output, so `print` will need to draw digits on the display using the built-in font sprites. The emulator supports everything this needs (`FX33` to split a number into decimal digits, `FX29` to find a digit's glyph), and `../examples/sum.asm` shows the pattern by hand: compute a number, split it into digits, draw each one.

## Later

- Loops (`while`)
- Conditionals (`if`)
- Functions
