# ROMs

Drop CHIP-8 `.ch8` ROM files here to run them with the emulator.

`zero.ch8` is a tiny hand-made test ROM (11 bytes) that draws the CHIP-8 "0" glyph in the top-left corner and then loops forever. It's a quick check that drawing works.

Other than that, I'm not committing ROMs to the repo. Look for freely licensed or public-domain test ROMs (there are community test suites that check each opcode), and check each ROM's license before adding it.

Run one with:

```
make
./chip8 roms/<name>.ch8
```
