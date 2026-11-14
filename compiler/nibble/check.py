"""Semantic checks and constant folding.

All arithmetic is on 8-bit unsigned numbers and wraps around (200 + 100 is 44),
because that is what a CHIP-8 register does.
"""

from . import tree
from .errors import NibbleError
from .parser import RESERVED


def check(program):
    """Find the program's variables, in order of first assignment.

    A variable must be assigned before it is read (in the order the code is
    written), so every read sees a value the program put there.
    """
    variables = []

    def expr(e):
        if isinstance(e, tree.Var):
            if e.name not in variables:
                raise NibbleError(
                    f"'{e.name}' is used before it is given a value", e.line, e.col
                )
        elif isinstance(e, tree.Binary):
            expr(e.left)
            expr(e.right)
        elif isinstance(e, tree.Unary):
            expr(e.operand)
        elif isinstance(e, tree.Call):
            for a in e.args:
                expr(a)

    def stmts(body):
        for s in body:
            if isinstance(s, tree.Assign):
                expr(s.value)
                if s.name in RESERVED:  # the parser stops this; belt and braces
                    raise NibbleError(f"'{s.name}' is a reserved word", s.line, s.col)
                if s.name not in variables:
                    variables.append(s.name)
            elif isinstance(s, tree.If):
                expr(s.cond)
                stmts(s.then)
                if s.otherwise:
                    stmts(s.otherwise)
            elif isinstance(s, tree.While):
                expr(s.cond)
                stmts(s.body)
            elif isinstance(s, tree.CallStmt):
                for a in s.args:
                    expr(a)

    stmts(program.statements)
    return variables


# ---- constant folding ------------------------------------------------------

_ARITH = {
    "+": lambda a, b: (a + b) & 0xFF,
    "-": lambda a, b: (a - b) & 0xFF,
    "*": lambda a, b: (a * b) & 0xFF,
    "&": lambda a, b: a & b,
    "|": lambda a, b: a | b,
    "^": lambda a, b: a ^ b,
    "==": lambda a, b: int(a == b),
    "!=": lambda a, b: int(a != b),
    "<": lambda a, b: int(a < b),
    "<=": lambda a, b: int(a <= b),
    ">": lambda a, b: int(a > b),
    ">=": lambda a, b: int(a >= b),
}


def fold(e):
    """Evaluate the parts of an expression that don't depend on anything running."""
    if isinstance(e, tree.Binary):
        left, right = fold(e.left), fold(e.right)
        ln, rn = isinstance(left, tree.Num), isinstance(right, tree.Num)
        if e.op == "and":
            if ln and left.value == 0:
                return tree.Num(0, e.line, e.col)  # right side is never evaluated
            if ln and rn:
                return tree.Num(int(right.value != 0), e.line, e.col)
        elif e.op == "or":
            if ln and left.value != 0:
                return tree.Num(1, e.line, e.col)
            if ln and rn:
                return tree.Num(int(right.value != 0), e.line, e.col)
        elif ln and rn:
            return tree.Num(_ARITH[e.op](left.value, right.value), e.line, e.col)
        return tree.Binary(e.op, left, right, e.line, e.col)
    if isinstance(e, tree.Unary):
        operand = fold(e.operand)
        if isinstance(operand, tree.Num):
            return tree.Num(int(operand.value == 0), e.line, e.col)
        return tree.Unary(e.op, operand, e.line, e.col)
    if isinstance(e, tree.Call):
        return tree.Call(e.name, [fold(a) for a in e.args], e.line, e.col)
    return e
