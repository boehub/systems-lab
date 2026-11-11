; Sketch for the draft custom instruction set in ../docs/instruction-set.md.
; This is NOT CHIP-8 and chip8asm.py can't assemble it. See zero.asm for a
; program that runs today.
; Computes 2 + 3 * 4 and prints it.

    LDI  R0, 3
    LDI  R1, 4
    MUL  R0, R1        ; R0 = 3 * 4
    LDI  R2, 2
    ADD  R0, R2        ; R0 = 12 + 2
    PRINT R0
    HALT
