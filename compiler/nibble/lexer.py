"""Lexer: turns source text into a list of tokens."""

import re
from collections import namedtuple

from .errors import NibbleError

# kind is one of: NUM, NAME, OP, EOF
Token = namedtuple("Token", "kind text value line col")

_NUMBER = re.compile(r"0[xX][0-9a-fA-F]+|0[bB][01]+|[0-9]+")
_NAME = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")
_TWO_CHAR = ("==", "!=", "<=", ">=")
_ONE_CHAR = "+-*&|^<>=(){},;"


def tokenize(source):
    tokens = []
    i, line, col = 0, 1, 1
    n = len(source)

    def advance(count):
        nonlocal i, col
        i += count
        col += count

    while i < n:
        ch = source[i]
        if ch == "\n":
            i += 1
            line += 1
            col = 1
        elif ch in " \t\r":
            advance(1)
        elif source.startswith("//", i):
            while i < n and source[i] != "\n":
                advance(1)
        elif ch.isdigit():
            m = _NUMBER.match(source, i)
            end = m.end()
            if end < n and (source[end].isalnum() or source[end] == "_"):
                raise NibbleError(f"bad number '{_word(source, i)}'", line, col)
            tokens.append(Token("NUM", m.group(), int(m.group(), 0), line, col))
            advance(end - i)
        elif ch.isalpha() or ch == "_":
            m = _NAME.match(source, i)
            tokens.append(Token("NAME", m.group(), None, line, col))
            advance(m.end() - i)
        elif source[i : i + 2] in _TWO_CHAR:
            tokens.append(Token("OP", source[i : i + 2], None, line, col))
            advance(2)
        elif ch in _ONE_CHAR:
            tokens.append(Token("OP", ch, None, line, col))
            advance(1)
        else:
            raise NibbleError(f"unexpected character {ch!r}", line, col)

    tokens.append(Token("EOF", "", None, line, col))
    return tokens


def _word(source, i):
    j = i
    while j < len(source) and (source[j].isalnum() or source[j] == "_"):
        j += 1
    return source[i:j]
