#!/usr/bin/env python3
"""nibc: the Nibble compiler.

    nibc.py prog.nib -o prog.ch8        compile to a ROM
    nibc.py prog.nib -S                 print the generated assembly instead
    nibc.py prog.nib --format c -o rom.h

Run the ROM with ../cpu-emulator/chip8 prog.ch8.
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from chip8asm import AsmError, assemble, to_c_header  # noqa: E402
from nibble import NibbleError, compile_to_asm  # noqa: E402


def main(argv=None):
    ap = argparse.ArgumentParser(description="Compile a Nibble program for CHIP-8.")
    ap.add_argument("source", help="program (.nib)")
    ap.add_argument("-o", "--output", help="output file (required unless -S)")
    ap.add_argument("-S", dest="asm_only", action="store_true",
                    help="print the generated assembly and stop")
    ap.add_argument("--format", choices=("bin", "c"), default="bin",
                    help="bin: raw .ch8 ROM (default); c: C/C++ header with a byte array")
    ap.add_argument("--name", default="program", help="array name for --format c")
    args = ap.parse_args(argv)

    try:
        with open(args.source, encoding="utf-8") as f:
            source = f.read()
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    try:
        asm = compile_to_asm(source)
    except NibbleError as e:
        print(f"{args.source}: error: {e.render(source)}", file=sys.stderr)
        return 1

    if args.asm_only:
        sys.stdout.write(asm)
        return 0
    if not args.output:
        ap.error("an output file is required (-o FILE), or use -S")

    try:
        rom = assemble(asm)
    except AsmError as e:  # a compiler bug, or a program too big for CHIP-8 memory
        print(f"{args.source}: error: generated code didn't assemble: {e}", file=sys.stderr)
        return 1

    if args.format == "c":
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(to_c_header(rom, args.name, os.path.basename(args.source)))
    else:
        with open(args.output, "wb") as f:
            f.write(rom)
    print(f"{args.source}: {len(rom)} bytes -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
