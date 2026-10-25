; Sketch of the planned assembly syntax (not assembled yet).
; Computes 2 + 3 * 4 and prints it.

    LDI  R0, 3
    LDI  R1, 4
    MUL  R0, R1        ; R0 = 3 * 4
    LDI  R2, 2
    ADD  R0, R2        ; R0 = 12 + 2
    PRINT R0
    HALT
