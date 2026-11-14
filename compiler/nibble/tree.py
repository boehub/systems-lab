"""Syntax tree for Nibble."""

from dataclasses import dataclass, field
from typing import List, Optional


# ---- expressions -----------------------------------------------------------

@dataclass
class Num:
    value: int
    line: int = 0
    col: int = 0


@dataclass
class Var:
    name: str
    line: int = 0
    col: int = 0


@dataclass
class Unary:  # only 'not'
    op: str
    operand: object
    line: int = 0
    col: int = 0


@dataclass
class Binary:
    op: str
    left: object
    right: object
    line: int = 0
    col: int = 0


@dataclass
class Call:  # key(), pressed(k), rand()
    name: str
    args: list
    line: int = 0
    col: int = 0


# ---- statements ------------------------------------------------------------

@dataclass
class Assign:
    name: str
    value: object
    line: int = 0
    col: int = 0


@dataclass
class If:
    cond: object
    then: list
    otherwise: Optional[list] = None
    line: int = 0
    col: int = 0


@dataclass
class While:
    cond: object
    body: list
    line: int = 0
    col: int = 0


@dataclass
class CallStmt:  # print(e), newline(), clear(), at(x, y), wait(n)
    name: str
    args: list
    line: int = 0
    col: int = 0


@dataclass
class Program:
    statements: List[object] = field(default_factory=list)
