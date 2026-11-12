; Draws three "0" glyphs in a row, using a subroutine and a loop.
; Uses CALL, RET, SE (skip), JP, ADD, LD, and DRW.

        LD   V0, 0          ; x position
        LD   V1, 0          ; y position
        LD   V2, 3          ; glyphs left to draw

loop:   CALL draw_zero      ; draw one glyph at (V0, V1)
        ADD  V0, 8          ; move 8 pixels to the right
        ADD  V2, 0xFF       ; V2 -= 1 (adding 255 wraps around)
        SE   V2, 0          ; skip the jump once the counter hits zero
        JP   loop

done:   JP   done           ; spin here

draw_zero:
        LD   I, glyph
        DRW  V0, V1, 5
        RET

glyph:  DB   0xF0, 0x90, 0x90, 0x90, 0xF0
