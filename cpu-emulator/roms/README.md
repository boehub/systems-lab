# ROMs

Drop CHIP-8 `.ch8` ROM files here to run them with the emulator.

`zero.ch8` is a tiny hand-made test ROM (11 bytes) that draws the CHIP-8 "0" glyph in the top-left corner and then loops forever. It's a quick check that drawing works.

Other than that, I'm not committing ROMs to the repo. `make conformance` (in `..`) downloads the community CHIP-8 test ROMs, checks them against known fingerprints and runs them, without storing them here. If you add ROMs of your own, check each one's license first.

Run one with:

```
make
./chip8 roms/<name>.ch8
```
