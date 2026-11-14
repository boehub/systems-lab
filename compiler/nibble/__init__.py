"""Nibble: a tiny language that compiles to CHIP-8.

    from nibble import compile_to_asm, compile_to_rom
"""

import os
import sys

from .check import check
from .codegen import generate
from .errors import NibbleError
from .parser import parse

__all__ = ["compile_to_asm", "compile_to_rom", "NibbleError"]


def compile_to_asm(source):
    """Nibble source -> CHIP-8 assembly text."""
    program = parse(source)
    variables = check(program)
    return generate(program, variables)


def compile_to_rom(source):
    """Nibble source -> ROM bytes (needs chip8asm.py from the folder above)."""
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if here not in sys.path:
        sys.path.insert(0, here)
    from chip8asm import assemble

    return assemble(compile_to_asm(source))
