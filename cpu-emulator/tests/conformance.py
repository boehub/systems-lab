#!/usr/bin/env python3
"""Check the emulator against an independent reference: Timendus's community
CHIP-8 test suite (https://github.com/Timendus/chip8-test-suite).

Each of those ROMs runs a batch of tests on the machine and draws a checkmark
or a cross for each result. This script downloads three of them, runs them in
the emulator, and counts the marks on the final screen.

    make conformance

Needs network access the first time (ROMs are cached in tests/.conformance-cache,
which git ignores; the ROMs are not part of this repository). Each download is
checked against a SHA-256 fingerprint so that a changed ROM is noticed instead
of silently changing the results.
"""

import hashlib
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "chip8")
CACHE = os.path.join(HERE, ".conformance-cache")
BASE = "https://raw.githubusercontent.com/Timendus/chip8-test-suite/main/bin/"

sys.path.insert(0, HERE)
from test_interactive import Session  # noqa: E402

ROMS = {
    "3-corax+.ch8": "1c7e14eae14d6d5e1e47693804110354cbc4081defe4e6e5d9167c25ffc7b4b0",
    "4-flags.ch8": "f00ddadd37bc878473de0c8f16faecf9985dea39036a3a796d551bc9fec47cfa",
    "5-quirks.ch8": "d839350268a3e73c7a16562b3d23c85aa1b92a567f5f61bd6727b1ea44635679",
}

# The marks the ROMs draw, as bit patterns of the three leftmost sprite columns.
CHECK_3ROW = [0b101, 0b110, 0b100]  # "# #" / "##" / "#"
CROSS_3ROW = [0b101, 0b010, 0b101]  # an X
CHECK_4ROW = [0b000] + CHECK_3ROW   # corax+ draws its check with a blank first row


def fetch(name):
    """Return the path of a verified copy of the ROM, downloading it if needed."""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if not os.path.exists(path):
        try:
            with urllib.request.urlopen(BASE + name.replace("+", "%2B"), timeout=30) as r:
                data = r.read()
        except OSError as e:
            sys.exit(f"could not download {name}: {e}\n(needs network the first time)")
        with open(path, "wb") as f:
            f.write(data)
    with open(path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    if digest != ROMS[name]:
        os.remove(path)
        sys.exit(
            f"{name} doesn't match its expected fingerprint (the upstream ROM may "
            f"have changed), so its results can't be compared.\n  expected {ROMS[name]}\n  got      {digest}"
        )
    return path


def count(pixels, pattern, x_range=(0, 62), y_range=None):
    """How many times a mark appears (comparing only the 3 leftmost columns)."""
    rows = len(pattern)
    ys = y_range or (0, 32 - rows + 1)
    n = 0
    for y in range(*ys):
        for x in range(*x_range):
            if all(
                sum(pixels[y + r][x + b] << (2 - b) for b in range(3)) == pattern[r]
                for r in range(rows)
            ):
                n += 1
    return n


def headless_pixels(rom, cycles):
    """Run without a terminal and read the final screen."""
    r = subprocess.run(
        [BIN, "--headless", "--cycles", str(cycles), rom],
        capture_output=True, text=True, timeout=120,
    )
    lines = r.stdout.splitlines()[:32]
    if r.returncode != 0:
        sys.exit(f"{os.path.basename(rom)}: emulator stopped: {r.stderr.strip()}")
    return [[1 if c == "#" else 0 for c in line.ljust(64)] for line in lines]


def screen_pixels(frame_lines):
    """Turn the terminal's half-block picture back into a 64x32 pixel grid."""
    px = []
    for line in frame_lines[:16]:
        px.append([1 if ch in "█▀" else 0 for ch in line.ljust(64)])
        px.append([1 if ch in "█▄" else 0 for ch in line.ljust(64)])
    return px


def run_quirks(rom):
    """Run the quirks test, choosing the CHIP-8 platform (key 1) like a user would."""
    s = Session(args=["--speed", "6000", "--vip"], rom_path=rom)
    try:
        start = time.monotonic()
        verdicts = (0, 0)
        # Press 1 every 0.3 s: the menu takes a few seconds to draw because
        # --vip limits drawing to one sprite per frame.
        while time.monotonic() - start < 60:
            s.send("1")
            s.settle(0.3)
            lines = s.text().split("\x1b[H")[-1].split("\x1b")[0].split("\r\n")
            if len(lines) >= 16:
                px = screen_pixels(lines)
                # The six verdict marks sit in the right-hand column.
                verdicts = (
                    count(px, CHECK_3ROW[0:3], (54, 62), (0, 30)),
                    count(px, CROSS_3ROW, (54, 62), (0, 30)),
                )
                if sum(verdicts) >= 6:
                    break
        s.send("\x1b")
        s.finish()
        return verdicts
    finally:
        s.close()


def main():
    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found: run `make` first (or `make conformance`)")

    results = []

    px = headless_pixels(fetch("3-corax+.ch8"), 200000)
    checks = count(px, CHECK_4ROW)
    results.append(("corax+ opcode test", f"{checks} of 22 checkmarks", checks == 22))

    px = headless_pixels(fetch("4-flags.ch8"), 300000)
    checks, crosses = count(px, CHECK_3ROW), count(px, CROSS_3ROW)
    results.append(("flags test", f"{checks} of 49 checkmarks, {crosses} crosses",
                    checks == 49 and crosses == 0))

    checks, crosses = run_quirks(fetch("5-quirks.ch8"))
    results.append(("quirks test (CHIP-8, --vip)", f"{checks} of 6 checkmarks, {crosses} crosses",
                    checks == 6 and crosses == 0))

    width = max(len(name) for name, _, _ in results)
    for name, detail, ok in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name:<{width}}  {detail}")
    return 0 if all(ok for _, _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main())
