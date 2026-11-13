; Adds up 1 + 2 + ... + 10 and draws the answer (55) using the built-in font.
; Uses register ADD, a loop, BCD (LD B), register load (LD Vx,[I]),
; font lookup (LD F), and DRW.

        LD   V3, 10         ; n = 10
        LD   V4, 0          ; sum = 0

sum:    ADD  V4, V3         ; sum += n
        ADD  V3, 0xFF       ; n -= 1 (adding 255 wraps around)
        SE   V3, 0          ; stop once n reaches 0
        JP   sum

        LD   I, digits      ; V4 holds 55: split it into decimal digits
        LD   B, V4          ; digits[0..2] = hundreds, tens, ones
        LD   V2, [I]        ; V0 = hundreds, V1 = tens, V2 = ones

        LD   V6, 0          ; x position
        LD   V7, 0          ; y position
        LD   F, V1          ; I -> font glyph for the tens digit
        DRW  V6, V7, 5
        ADD  V6, 5          ; next digit goes 5 pixels to the right
        LD   F, V2          ; I -> font glyph for the ones digit
        DRW  V6, V7, 5

done:   JP   done           ; spin here

digits: DB   0, 0, 0        ; scratch space for the three digits
