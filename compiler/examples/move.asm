; Move a ball around the screen with the keypad diamond:
;   CHIP-8 key 2 = up, 8 = down, 4 = left, 6 = right
;   (on a QWERTY keyboard those are the keys 2, S, Q and E)
; Uses key tests (SKNP), the delay timer for pacing, AND for wrap-around, and DRW.

        LD   V0, 28         ; x
        LD   V1, 14         ; y

frame:  LD   I, ball
        DRW  V0, V1, 4      ; draw the ball

        LD   V5, 2          ; wait 2 ticks (1/30 s) so the speed doesn't depend on the CPU
        LD   DT, V5
wait:   LD   V5, DT
        SE   V5, 0
        JP   wait

        DRW  V0, V1, 4      ; erase it (drawing twice cancels out)

        LD   V2, 2
        SKNP V2             ; skip the next line unless key 2 is down
        ADD  V1, 0xFF       ; y -= 1
        LD   V2, 8
        SKNP V2
        ADD  V1, 1          ; y += 1
        LD   V2, 4
        SKNP V2
        ADD  V0, 0xFF       ; x -= 1
        LD   V2, 6
        SKNP V2
        ADD  V0, 1          ; x += 1

        LD   V3, 0x3F       ; keep x in 0..63 and y in 0..31 (wrap around the edges)
        AND  V0, V3
        LD   V3, 0x1F
        AND  V1, V3
        JP   frame

ball:   DB   0x60, 0xF0, 0xF0, 0x60
