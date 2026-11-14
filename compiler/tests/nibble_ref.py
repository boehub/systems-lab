"""A reference interpreter for Nibble, used to test the compiler.

It evaluates the syntax tree directly with 8-bit wrap-around arithmetic. It
shares the parser with the compiler but nothing else, so a bug in code
generation shows up as a difference between this and the real emulator.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from nibble import tree  # noqa: E402
from nibble.parser import parse  # noqa: E402


class Output:
    """What a program drew: (x, y, number) for each print."""

    def __init__(self):
        self.prints = []


def run(source, max_steps=200000):
    prog = parse(source)
    env, out = {}, Output()
    cursor = [0, 0]
    steps = [0]

    def tick():
        steps[0] += 1
        if steps[0] > max_steps:
            raise RuntimeError("program ran too long")

    def ev(e):
        if isinstance(e, tree.Num):
            return e.value
        if isinstance(e, tree.Var):
            return env[e.name]
        if isinstance(e, tree.Unary):
            return int(ev(e.operand) == 0)
        if isinstance(e, tree.Call):
            if e.name == "pressed":
                ev(e.args[0])
                return 0  # no keys held
            raise RuntimeError(f"{e.name}() is not deterministic")
        op = e.op
        if op == "and":
            return int(ev(e.left) != 0 and ev(e.right) != 0)
        if op == "or":
            return int(ev(e.left) != 0 or ev(e.right) != 0)
        a, b = ev(e.left), ev(e.right)
        return {
            "+": lambda: (a + b) & 255,
            "-": lambda: (a - b) & 255,
            "*": lambda: (a * b) & 255,
            "&": lambda: a & b,
            "|": lambda: a | b,
            "^": lambda: a ^ b,
            "==": lambda: int(a == b),
            "!=": lambda: int(a != b),
            "<": lambda: int(a < b),
            "<=": lambda: int(a <= b),
            ">": lambda: int(a > b),
            ">=": lambda: int(a >= b),
        }[op]()

    def block(stmts):
        for s in stmts:
            tick()
            if isinstance(s, tree.Assign):
                env[s.name] = ev(s.value)
            elif isinstance(s, tree.If):
                if ev(s.cond) != 0:
                    block(s.then)
                elif s.otherwise:
                    block(s.otherwise)
            elif isinstance(s, tree.While):
                while ev(s.cond) != 0:
                    tick()
                    block(s.body)
            elif s.name == "print":
                v = ev(s.args[0])
                out.prints.append((cursor[0], cursor[1], v))
                cursor[0] += 5 * len(str(v)) + 5
            elif s.name == "at":
                cursor[0], cursor[1] = ev(s.args[0]), ev(s.args[1])
            elif s.name == "newline":
                cursor[0], cursor[1] = 0, cursor[1] + 6
            elif s.name == "clear":
                cursor[0], cursor[1] = 0, 0
                out.prints.clear()
            elif s.name == "wait":
                ev(s.args[0])

    block(prog.statements)
    return out
