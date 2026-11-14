"""Parser: tokens -> syntax tree (recursive descent).

Operator precedence, loosest to tightest:

    or
    and
    not
    == != < <= > >=        (cannot be chained)
    |
    ^
    &
    + -
    *
"""

from . import tree
from .errors import NibbleError
from .lexer import tokenize

KEYWORDS = {"if", "else", "while", "and", "or", "not"}
EXPR_BUILTINS = {"key": 0, "pressed": 1, "rand": 0}  # name -> argument count
STMT_BUILTINS = {"print": 1, "newline": 0, "clear": 0, "at": 2, "wait": 1}
RESERVED = KEYWORDS | set(EXPR_BUILTINS) | set(STMT_BUILTINS)

_COMPARE = ("==", "!=", "<", "<=", ">", ">=")


def parse(source):
    return _Parser(tokenize(source)).program()


class _Parser:
    def __init__(self, tokens):
        self.toks = tokens
        self.pos = 0

    # ---- token helpers -----------------------------------------------------

    @property
    def tok(self):
        return self.toks[self.pos]

    def at_op(self, *ops):
        return self.tok.kind == "OP" and self.tok.text in ops

    def at_word(self, *words):
        return self.tok.kind == "NAME" and self.tok.text in words

    def next(self):
        t = self.tok
        self.pos += 1
        return t

    def error(self, message, tok=None):
        tok = tok or self.tok
        return NibbleError(message, tok.line, tok.col)

    def describe(self, tok):
        return "end of file" if tok.kind == "EOF" else f"'{tok.text}'"

    def expect_op(self, op, why=""):
        if not self.at_op(op):
            raise self.error(f"expected '{op}'{why}, found {self.describe(self.tok)}")
        return self.next()

    # ---- statements --------------------------------------------------------

    def program(self):
        stmts = []
        self.skip_semicolons()
        while self.tok.kind != "EOF":
            stmts.append(self.statement())
            self.skip_semicolons()
        return tree.Program(stmts)

    def skip_semicolons(self):  # ';' is optional, so stray ones are harmless
        while self.at_op(";"):
            self.next()

    def block(self):
        self.expect_op("{", " to start a block")
        stmts = []
        self.skip_semicolons()
        while not self.at_op("}"):
            if self.tok.kind == "EOF":
                raise self.error("missing '}' before end of file")
            stmts.append(self.statement())
            self.skip_semicolons()
        self.next()
        return stmts

    def statement(self):
        t = self.tok
        if t.kind != "NAME":
            raise self.error(f"expected a statement, found {self.describe(t)}")
        if t.text == "if":
            return self.if_statement()
        if t.text == "while":
            self.next()
            cond = self.expression()
            return tree.While(cond, self.block(), t.line, t.col)
        if t.text in KEYWORDS:
            raise self.error(f"'{t.text}' can't start a statement")
        if t.text in STMT_BUILTINS:
            return self.call_statement()
        if t.text in EXPR_BUILTINS:
            raise self.error(
                f"{t.text}() gives a value; use it in an expression, e.g. x = {t.text}()"
            )
        self.next()
        if self.at_op("("):
            raise self.error(f"unknown function '{t.text}'", t)
        if not self.at_op("="):
            raise self.error(
                f"expected '=' after '{t.text}', found {self.describe(self.tok)}"
            )
        self.next()
        return tree.Assign(t.text, self.expression(), t.line, t.col)

    def if_statement(self):
        t = self.next()
        cond = self.expression()
        then = self.block()
        otherwise = None
        if self.at_word("else"):
            self.next()
            if self.at_word("if"):
                otherwise = [self.if_statement()]
            else:
                otherwise = self.block()
        return tree.If(cond, then, otherwise, t.line, t.col)

    def call_statement(self):
        t = self.next()
        args = self.arguments(t, STMT_BUILTINS[t.text])
        return tree.CallStmt(t.text, args, t.line, t.col)

    def arguments(self, name_tok, count):
        self.expect_op("(", f" after {name_tok.text}")
        args = []
        if not self.at_op(")"):
            args.append(self.expression())
            while self.at_op(","):
                self.next()
                args.append(self.expression())
        self.expect_op(")", f" to close {name_tok.text}(")
        if len(args) != count:
            s = "" if count == 1 else "s"
            raise self.error(
                f"{name_tok.text}() takes {count} argument{s}, got {len(args)}", name_tok
            )
        return args

    # ---- expressions -------------------------------------------------------

    def expression(self):
        return self.or_expr()

    def or_expr(self):
        left = self.and_expr()
        while self.at_word("or"):
            t = self.next()
            left = tree.Binary("or", left, self.and_expr(), t.line, t.col)
        return left

    def and_expr(self):
        left = self.not_expr()
        while self.at_word("and"):
            t = self.next()
            left = tree.Binary("and", left, self.not_expr(), t.line, t.col)
        return left

    def not_expr(self):
        if self.at_word("not"):
            t = self.next()
            return tree.Unary("not", self.not_expr(), t.line, t.col)
        return self.compare()

    def compare(self):
        left = self.bit_or()
        if self.at_op(*_COMPARE):
            t = self.next()
            left = tree.Binary(t.text, left, self.bit_or(), t.line, t.col)
            if self.at_op(*_COMPARE):
                raise self.error("comparisons can't be chained; combine them with 'and'")
        return left

    def _left_assoc(self, ops, operand):
        left = operand()
        while self.at_op(*ops):
            t = self.next()
            left = tree.Binary(t.text, left, operand(), t.line, t.col)
        return left

    def bit_or(self):
        return self._left_assoc(("|",), self.bit_xor)

    def bit_xor(self):
        return self._left_assoc(("^",), self.bit_and)

    def bit_and(self):
        return self._left_assoc(("&",), self.additive)

    def additive(self):
        return self._left_assoc(("+", "-"), self.term)

    def term(self):
        return self._left_assoc(("*",), self.primary)

    def primary(self):
        t = self.tok
        if t.kind == "NUM":
            self.next()
            if t.value > 255:
                raise self.error(f"{t.text} is too big: numbers go up to 255", t)
            return tree.Num(t.value, t.line, t.col)
        if t.kind == "NAME":
            if t.text in EXPR_BUILTINS:
                self.next()
                args = self.arguments(t, EXPR_BUILTINS[t.text])
                return tree.Call(t.text, args, t.line, t.col)
            if t.text == "not":
                raise self.error("'not' can't follow an operator; put it in parentheses: (not x)")
            if t.text in RESERVED:
                raise self.error(f"'{t.text}' can't be used here")
            self.next()
            if self.at_op("("):
                raise self.error(f"unknown function '{t.text}'", t)
            return tree.Var(t.text, t.line, t.col)
        if self.at_op("("):
            self.next()
            e = self.expression()
            self.expect_op(")", " to close the parenthesis")
            return e
        raise self.error(f"expected a value, found {self.describe(t)}")
