#!/usr/bin/env python3
"""A two-pass assembler for CHIP-8.

Turns text like

    start:  LD I, sprite
            DRW V0, V1, 5
    loop:   JP loop
    sprite: DB 0xF0, 0x90, 0x90, 0x90, 0xF0

into a ROM that runs on the emulator in ../cpu-emulator.

Syntax follows the usual CHIP-8 mnemonics. Mnemonics and registers are
case-insensitive, labels are case-sensitive, comments start with ';'.
Programs are placed at 0x200, where CHIP-8 programs start.
"""

import argparse
import re
import sys

ORIGIN = 0x200
MEMORY_SIZE = 4096

_REG = re.compile(r"^v([0-9a-f])$", re.IGNORECASE)
_LABEL = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
_KEYWORDS = {"I", "DT", "ST", "K", "F", "B", "[I]"}


class AsmError(Exception):
    """An error in the source program, with its line number."""

    def __init__(self, message, line_no=None):
        self.line_no = line_no
        super().__init__(f"line {line_no}: {message}" if line_no else message)


# ---------------------------------------------------------------- operands


def _classify(text):
    """Return ('reg', n), ('kw', NAME) or ('val', text) for one operand."""
    m = _REG.match(text)
    if m:
        return ("reg", int(m.group(1), 16))
    if text.upper() in _KEYWORDS:
        return ("kw", text.upper())
    return ("val", text)


def _is_label_name(name):
    return bool(_LABEL.match(name)) and _classify(name)[0] == "val"


def _number(text):
    """Parse 0x1F, 0b101 or 123. Returns None if it isn't a number."""
    try:
        return int(text, 0)
    except ValueError:
        return None


def _value(text, labels, line_no, bits, what):
    """Resolve a number or label and check it fits in `bits` bits."""
    n = _number(text)
    if n is None:
        if not _is_label_name(text):
            raise AsmError(f"expected {what}, got '{text}'", line_no)
        if text not in labels:
            raise AsmError(f"undefined label '{text}'", line_no)
        n = labels[text]
    if not 0 <= n < (1 << bits):
        raise AsmError(f"{what} {text} out of range (0..{(1 << bits) - 1})", line_no)
    return n


# ------------------------------------------------------------ source parsing


def _split_operands(rest):
    return [p.strip() for p in rest.split(",")] if rest.strip() else []


def _parse(source):
    """Pass 1: split into items and record label addresses.

    Returns (items, labels) where each item is (line_no, mnemonic, operands,
    address). Directives DB/DW are kept as items too.
    """
    items = []
    labels = {}
    address = ORIGIN

    for line_no, raw in enumerate(source.splitlines(), start=1):
        line = raw.split(";", 1)[0].strip()
        # Peel off any number of leading "label:" definitions.
        while True:
            m = re.match(r"^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*(.*)$", line)
            if not m:
                break
            name, line = m.group(1), m.group(2)
            if not _is_label_name(name):
                raise AsmError(f"'{name}' can't be used as a label", line_no)
            if name in labels:
                raise AsmError(f"duplicate label '{name}'", line_no)
            labels[name] = address
        if not line:
            continue

        parts = line.split(None, 1)
        mnemonic = parts[0].upper()
        operands = _split_operands(parts[1] if len(parts) > 1 else "")

        if mnemonic == "DB":
            size = len(operands)
        elif mnemonic == "DW":
            size = 2 * len(operands)
        else:
            size = 2
        if mnemonic in ("DB", "DW") and not operands:
            raise AsmError(f"{mnemonic} needs at least one value", line_no)

        items.append((line_no, mnemonic, operands, address))
        address += size
        if address > MEMORY_SIZE:
            raise AsmError("program is too large for CHIP-8 memory", line_no)

    return items, labels


# ------------------------------------------------------------------ encoding


def _encode(mnemonic, ops, labels, line_no):
    """Encode one instruction to a 16-bit opcode."""
    kinds = [_classify(o) for o in ops]

    def need(count):
        if len(ops) != count:
            raise AsmError(
                f"{mnemonic} takes {count} operand(s), got {len(ops)}", line_no
            )

    def reg(i):
        if kinds[i][0] != "reg":
            raise AsmError(f"operand {i + 1} of {mnemonic} must be a register", line_no)
        return kinds[i][1]

    def val(i, bits, what):
        if kinds[i][0] != "val":
            raise AsmError(f"operand {i + 1} of {mnemonic} must be {what}", line_no)
        return _value(ops[i], labels, line_no, bits, what)

    def is_reg(i):
        return kinds[i][0] == "reg"

    def is_kw(i, name):
        return kinds[i] == ("kw", name)

    m = mnemonic

    if m == "CLS":
        need(0)
        return 0x00E0
    if m == "RET":
        need(0)
        return 0x00EE
    if m == "SYS":
        need(1)
        return 0x0000 | val(0, 12, "an address")
    if m == "CALL":
        need(1)
        return 0x2000 | val(0, 12, "an address")

    if m == "JP":
        if len(ops) == 1:
            return 0x1000 | val(0, 12, "an address")
        need(2)
        if kinds[0] != ("reg", 0):
            raise AsmError("two-operand JP must be 'JP V0, address'", line_no)
        return 0xB000 | val(1, 12, "an address")

    if m in ("SE", "SNE"):
        need(2)
        x = reg(0)
        base_byte, base_reg = (0x3000, 0x5000) if m == "SE" else (0x4000, 0x9000)
        if is_reg(1):
            return base_reg | (x << 8) | (reg(1) << 4)
        return base_byte | (x << 8) | val(1, 8, "a byte")

    if m == "LD":
        need(2)
        if is_kw(0, "I"):
            return 0xA000 | val(1, 12, "an address")
        if is_kw(0, "DT"):
            return 0xF015 | (reg(1) << 8)
        if is_kw(0, "ST"):
            return 0xF018 | (reg(1) << 8)
        if is_kw(0, "F"):
            return 0xF029 | (reg(1) << 8)
        if is_kw(0, "B"):
            return 0xF033 | (reg(1) << 8)
        if is_kw(0, "[I]"):
            return 0xF055 | (reg(1) << 8)
        x = reg(0)
        if is_kw(1, "DT"):
            return 0xF007 | (x << 8)
        if is_kw(1, "K"):
            return 0xF00A | (x << 8)
        if is_kw(1, "[I]"):
            return 0xF065 | (x << 8)
        if is_reg(1):
            return 0x8000 | (x << 8) | (reg(1) << 4)
        return 0x6000 | (x << 8) | val(1, 8, "a byte")

    if m == "ADD":
        need(2)
        if is_kw(0, "I"):
            return 0xF01E | (reg(1) << 8)
        x = reg(0)
        if is_reg(1):
            return 0x8004 | (x << 8) | (reg(1) << 4)
        return 0x7000 | (x << 8) | val(1, 8, "a byte")

    alu = {"OR": 0x1, "AND": 0x2, "XOR": 0x3, "SUB": 0x5, "SUBN": 0x7}
    if m in alu:
        need(2)
        return 0x8000 | (reg(0) << 8) | (reg(1) << 4) | alu[m]

    if m in ("SHR", "SHL"):
        if len(ops) not in (1, 2):
            raise AsmError(f"{m} takes 1 or 2 operands, got {len(ops)}", line_no)
        x = reg(0)
        y = reg(1) if len(ops) == 2 else 0
        return 0x8000 | (x << 8) | (y << 4) | (0x6 if m == "SHR" else 0xE)

    if m == "RND":
        need(2)
        return 0xC000 | (reg(0) << 8) | val(1, 8, "a byte")
    if m == "DRW":
        need(3)
        return 0xD000 | (reg(0) << 8) | (reg(1) << 4) | val(2, 4, "a row count (0-15)")
    if m == "SKP":
        need(1)
        return 0xE09E | (reg(0) << 8)
    if m == "SKNP":
        need(1)
        return 0xE0A1 | (reg(0) << 8)

    raise AsmError(f"unknown instruction '{mnemonic}'", line_no)


# ------------------------------------------------------------------ assemble


def assemble(source):
    """Assemble source text into ROM bytes."""
    items, labels = _parse(source)
    out = bytearray()

    for line_no, mnemonic, operands, _address in items:
        if mnemonic == "DB":
            for op in operands:
                out.append(_value(op, labels, line_no, 8, "a byte"))
        elif mnemonic == "DW":
            for op in operands:
                word = _value(op, labels, line_no, 16, "a 16-bit word")
                out += word.to_bytes(2, "big")
        else:
            opcode = _encode(mnemonic, operands, labels, line_no)
            out += opcode.to_bytes(2, "big")

    return bytes(out)


def to_c_header(rom, name, source_name):
    """Format a ROM as a C/C++ header, for embedding in firmware."""
    rows = []
    for i in range(0, len(rom), 12):
        rows.append("    " + ", ".join(f"0x{b:02X}" for b in rom[i : i + 12]) + ",")
    body = "\n".join(rows)
    return (
        f"// Generated by chip8asm.py from {source_name}. Do not edit.\n"
        "#pragma once\n\n"
        "#include <cstddef>\n"
        "#include <cstdint>\n\n"
        f"static const uint8_t {name}[] = {{\n{body}\n}};\n"
        f"static const size_t {name}_size = sizeof({name});\n"
    )


# ----------------------------------------------------------------------- CLI


def main(argv=None):
    parser = argparse.ArgumentParser(description="Assemble CHIP-8 source into a ROM.")
    parser.add_argument("source", help="assembly file (.asm)")
    parser.add_argument("-o", "--output", required=True, help="output file")
    parser.add_argument(
        "--format",
        choices=("bin", "c"),
        default="bin",
        help="bin: raw .ch8 ROM (default); c: C/C++ header with a byte array",
    )
    parser.add_argument(
        "--name", default="program", help="array name for --format c (default: program)"
    )
    args = parser.parse_args(argv)

    try:
        with open(args.source, encoding="utf-8") as f:
            rom = assemble(f.read())
    except OSError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except AsmError as e:
        print(f"{args.source}: error: {e}", file=sys.stderr)
        return 1

    if args.format == "c":
        text = to_c_header(rom, args.name, args.source.split("/")[-1])
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        with open(args.output, "wb") as f:
            f.write(rom)
    print(f"{args.source}: {len(rom)} bytes -> {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
