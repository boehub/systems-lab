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

On CHIP-8 there is no text output, so `print` will need to draw digits on the display using the built-in font sprites (the `FX29` opcode). The emulator doesn't support that yet.

## Later

- Loops (`while`)
- Conditionals (`if`)
- Functions
