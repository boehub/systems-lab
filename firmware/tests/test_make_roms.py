"""Tests for tools/make_roms.py, which builds the menu's program table.

    python3 tests/test_make_roms.py
"""

import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(os.path.dirname(HERE), "tools", "make_roms.py")
COMPILER = os.path.join(os.path.dirname(os.path.dirname(HERE)), "compiler")
sys.path.insert(0, COMPILER)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tools"))

from chip8asm import assemble  # noqa: E402
from nibble import compile_to_rom  # noqa: E402
import make_roms  # noqa: E402

EX = os.path.join(COMPILER, "examples")


def run(sources, **kw):
    """Run the tool; return (exit code, header text or None, stderr)."""
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "roms.h")
        r = subprocess.run([sys.executable, TOOL, "-o", out] + sources,
                           capture_output=True, text=True)
        header = open(out).read() if os.path.exists(out) else None
        return r.returncode, header, r.stderr


def parse(header):
    """The names, sizes and bytes that a generated header contains."""
    arrays = [bytes(int(b, 16) for b in re.findall(r"0x([0-9A-F]{2})", body))
              for body in re.findall(r"kRomData\d+\[\] = \{(.*?)\};", header, re.S)]
    table = re.findall(r'\{"([^"]*)", kRomData(\d+), (\d+)\}', header)
    count = int(re.search(r"kRomCount = (\d+)", header).group(1))
    return arrays, table, count


class MakeRomsTests(unittest.TestCase):
    def test_assembly_and_nibble_and_binary_programs(self):
        with tempfile.TemporaryDirectory() as d:
            raw = os.path.join(d, "raw_rom.ch8")
            with open(raw, "wb") as f:
                f.write(bytes([0x12, 0x00, 0xAB]))
            code, header, err = run([os.path.join(EX, "move.asm"),
                                     os.path.join(EX, "fib.nib"), raw])
        self.assertEqual(code, 0, err)
        arrays, table, count = parse(header)
        self.assertEqual(count, 3)
        self.assertEqual([t[0] for t in table], ["MOVE", "FIB", "RAW_ROM"])
        # The bytes are exactly what the assembler / compiler produce.
        with open(os.path.join(EX, "move.asm")) as f:
            self.assertEqual(arrays[0], assemble(f.read()))
        with open(os.path.join(EX, "fib.nib")) as f:
            self.assertEqual(arrays[1], compile_to_rom(f.read()))
        self.assertEqual(arrays[2], bytes([0x12, 0x00, 0xAB]))
        # ...and the table's sizes match the arrays.
        for i, ((name, index, size), data) in enumerate(zip(table, arrays)):
            self.assertEqual(int(index), i)
            self.assertEqual(int(size), len(data))

    def test_names_are_menu_safe_and_unique(self):
        with tempfile.TemporaryDirectory() as d:
            paths = []
            for name in ["my game.ch8", "a very long program name indeed.ch8", "dup.ch8"]:
                p = os.path.join(d, name)
                open(p, "wb").write(b"\x12\x00")
                paths.append(p)
            sub = os.path.join(d, "sub")
            os.mkdir(sub)
            dup = os.path.join(sub, "dup.ch8")
            open(dup, "wb").write(b"\x12\x00")
            code, header, err = run(paths + [dup])
        self.assertEqual(code, 0, err)
        names = [t[0] for t in parse(header)[1]]
        self.assertEqual(names[0], "MY-GAME")
        self.assertEqual(len(names[1]), 19)            # cut to what the menu shows
        self.assertEqual(names[2:], ["DUP", "DUP-2"])  # duplicates are told apart
        self.assertEqual(len(set(names)), len(names))

    def test_errors_name_the_file(self):
        with tempfile.TemporaryDirectory() as d:
            bad = os.path.join(d, "bad.asm")
            open(bad, "w").write("LD V0, nonsense_label_xyz\n")
            code, header, err = run([bad])
            self.assertNotEqual(code, 0)
            self.assertIn("bad.asm", err)

            badnib = os.path.join(d, "bad.nib")
            open(badnib, "w").write("print(x)\n")
            code, _, err = run([badnib])
            self.assertNotEqual(code, 0)
            self.assertIn("bad.nib", err)
            self.assertIn("used before", err)

            weird = os.path.join(d, "thing.txt")
            open(weird, "w").write("hi")
            code, _, err = run([weird])
            self.assertNotEqual(code, 0)
            self.assertIn(".txt", err)

            empty = os.path.join(d, "empty.ch8")
            open(empty, "wb").write(b"")
            code, _, err = run([empty])
            self.assertNotEqual(code, 0)
            self.assertIn("empty", err)

            big = os.path.join(d, "big.ch8")
            open(big, "wb").write(bytes(3585))
            code, _, err = run([big])
            self.assertNotEqual(code, 0)
            self.assertIn("too big", err)

            fits = os.path.join(d, "fits.ch8")
            open(fits, "wb").write(bytes([1]) * 3584)
            code, _, err = run([fits])
            self.assertEqual(code, 0, err)

    def test_menu_name(self):
        self.assertEqual(make_roms.menu_name("/x/y/move.asm"), "MOVE")
        self.assertEqual(make_roms.menu_name("a b.nib"), "A-B")
        self.assertEqual(make_roms.menu_name("pong_v2-final.ch8"), "PONG_V2-FINAL")


if __name__ == "__main__":
    unittest.main()
