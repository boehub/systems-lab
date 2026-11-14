"""Tests for the Nibble compiler.

Three layers:
  * unit tests for the lexer, parser, checker and error messages
  * tests of the generated assembly
  * end-to-end: compile, assemble, run in the real emulator, read the screen,
    and compare with a reference interpreter (including on random programs)

    python3 -m unittest discover -s tests
"""

import os
import random
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, ROOT)
sys.path.insert(0, HERE)

from chip8asm import assemble  # noqa: E402
from nibble import NibbleError, compile_to_asm, compile_to_rom  # noqa: E402
from nibble.check import check, fold  # noqa: E402
from nibble.lexer import tokenize  # noqa: E402
from nibble.parser import parse  # noqa: E402
from nibble import tree  # noqa: E402
import nibble_ref  # noqa: E402

EMULATOR = os.path.join(os.path.dirname(ROOT), "cpu-emulator", "chip8")
HAVE_EMULATOR = os.path.exists(EMULATOR)
# CI sets this so a missing emulator is a failure there, not a silent skip.
if os.environ.get("REQUIRE_EMULATOR") and not HAVE_EMULATOR:
    raise SystemExit(f"REQUIRE_EMULATOR is set but {EMULATOR} isn't built")
needs_emulator = unittest.skipUnless(HAVE_EMULATOR, "build cpu-emulator/chip8 first")


def error_for(source):
    with_err = None
    try:
        compile_to_asm(source)
    except NibbleError as e:
        with_err = e
    return with_err


class LexerTests(unittest.TestCase):
    def kinds(self, text):
        return [(t.kind, t.text) for t in tokenize(text)][:-1]

    def test_numbers_names_and_operators(self):
        self.assertEqual(
            self.kinds("x = 0x1F + 0b101 * 7"),
            [("NAME", "x"), ("OP", "="), ("NUM", "0x1F"), ("OP", "+"),
             ("NUM", "0b101"), ("OP", "*"), ("NUM", "7")],
        )
        self.assertEqual([t.value for t in tokenize("0x1F 0b101 42")[:3]], [31, 5, 42])

    def test_two_character_operators(self):
        self.assertEqual([t for _, t in self.kinds("a<=b>=c==d!=e<f>g")],
                         ["a", "<=", "b", ">=", "c", "==", "d", "!=", "e", "<", "f", ">", "g"])

    def test_comments_and_positions(self):
        toks = tokenize("// hello\n  x = 1 // trailing\ny")
        self.assertEqual([(t.text, t.line, t.col) for t in toks[:-1]],
                         [("x", 2, 3), ("=", 2, 5), ("1", 2, 7), ("y", 3, 1)])

    def test_bad_input(self):
        with self.assertRaisesRegex(NibbleError, "line 1: unexpected character '@'"):
            tokenize("x = @")
        with self.assertRaisesRegex(NibbleError, "bad number '12ab'"):
            tokenize("x = 12ab")


class ParserTests(unittest.TestCase):
    def expr(self, text):
        return parse(f"x = {text}").statements[0].value

    def shape(self, e):
        if isinstance(e, tree.Num):
            return e.value
        if isinstance(e, tree.Var):
            return e.name
        if isinstance(e, tree.Unary):
            return (e.op, self.shape(e.operand))
        if isinstance(e, tree.Call):
            return (e.name, *[self.shape(a) for a in e.args])
        return (e.op, self.shape(e.left), self.shape(e.right))

    def test_precedence(self):
        self.assertEqual(self.shape(self.expr("2 + 3 * 4")), ("+", 2, ("*", 3, 4)))
        self.assertEqual(self.shape(self.expr("(2 + 3) * 4")), ("*", ("+", 2, 3), 4))
        self.assertEqual(self.shape(self.expr("a - b - c")), ("-", ("-", "a", "b"), "c"))
        # bitwise operators bind tighter than comparisons (like Python, not C)
        self.assertEqual(self.shape(self.expr("a & 1 == 0")), ("==", ("&", "a", 1), 0))
        self.assertEqual(self.shape(self.expr("a | b ^ c & d")),
                         ("|", "a", ("^", "b", ("&", "c", "d"))))
        self.assertEqual(self.shape(self.expr("a or b and c")),
                         ("or", "a", ("and", "b", "c")))
        self.assertEqual(self.shape(self.expr("not a == b")), ("not", ("==", "a", "b")))
        self.assertEqual(self.shape(self.expr("a < b and c")), ("and", ("<", "a", "b"), "c"))

    def test_statements(self):
        prog = parse("""
            i = 0
            while i < 3 { i = i + 1 }
            if i == 3 { print(i) } else if i == 4 { clear() } else { newline() }
        """)
        self.assertEqual([type(s).__name__ for s in prog.statements],
                         ["Assign", "While", "If"])
        branch = prog.statements[2]
        self.assertIsInstance(branch.otherwise[0], tree.If)  # else-if chains
        self.assertEqual(branch.otherwise[0].otherwise[0].name, "newline")

    def test_calls(self):
        self.assertEqual(self.shape(self.expr("pressed(5) + rand()")),
                         ("+", ("pressed", 5), ("rand",)))

    def test_semicolons_are_optional(self):
        self.assertEqual(len(parse("x = 1; y = 2;; print(x);").statements), 3)

    def assertParseError(self, text, pattern):
        with self.assertRaisesRegex(NibbleError, pattern):
            parse(text)

    def test_errors(self):
        self.assertParseError("x = ", "expected a value, found end of file")
        self.assertParseError("x = 1 +", "expected a value")
        self.assertParseError("x = (1 + 2", r"expected '\)'")
        self.assertParseError("x = 256", "256 is too big")
        self.assertParseError("while 1 { x = 1", r"missing '}'")
        self.assertParseError("if 1 x = 1", r"expected '\{'")
        self.assertParseError("print(1, 2)", r"print\(\) takes 1 argument, got 2")
        self.assertParseError("at(1)", r"at\(\) takes 2 arguments, got 1")
        self.assertParseError("foo(1)", "unknown function 'foo'")
        self.assertParseError("x = foo(1)", "unknown function 'foo'")
        self.assertParseError("x 5", "expected '=' after 'x'")
        self.assertParseError("if = 1", "can't start a statement|expected")
        self.assertParseError("print = 1", "expected '\\('")
        self.assertParseError("x = while", "can't be used here")
        self.assertParseError("x = 1 + not 2", "put it in parentheses")
        self.assertParseError("x = a < b < c", "can't be chained")
        self.assertParseError("key()", "gives a value")
        self.assertParseError("5", "expected a statement")
        self.assertParseError("x = 1 }", "expected a statement")

    def test_error_reports_position_and_source(self):
        e = error_for("a = 1\nb = a +\n")
        self.assertEqual((e.line, e.col), (3, 1))
        e = error_for("x = 1\ny = x + $")
        self.assertEqual(e.line, 2)
        text = e.render("x = 1\ny = x + $")
        self.assertIn("y = x + $", text)
        self.assertIn("^", text)


class CheckTests(unittest.TestCase):
    def test_variables_in_order_of_first_assignment(self):
        self.assertEqual(check(parse("b = 1\na = b\nb = 2\nc = a")), ["b", "a", "c"])

    def test_use_before_assignment(self):
        e = error_for("print(x)")
        self.assertIn("'x' is used before it is given a value", str(e))
        self.assertEqual(e.line, 1)
        self.assertIn("used before", str(error_for("x = x + 1")))
        self.assertIn("used before", str(error_for("while i < 3 { i = i + 1 }")))
        # assigned inside an earlier branch counts (flow-insensitive, in text order)
        check(parse("if 1 { a = 1 }\nprint(a)"))

    def test_fold(self):
        def f(text):
            return fold(parse(f"x = {text}").statements[0].value)

        self.assertEqual(f("2 + 3 * 4").value, 14)
        self.assertEqual(f("200 + 100").value, 44)       # wraps like a register
        self.assertEqual(f("3 - 5").value, 254)
        self.assertEqual(f("16 * 16").value, 0)
        self.assertEqual(f("3 < 5").value, 1)
        self.assertEqual(f("not 0").value, 1)
        self.assertEqual(f("0 and a").value, 0)           # right side never runs
        self.assertEqual(f("1 or a").value, 1)
        self.assertEqual(f("7 and 9").value, 1)
        self.assertIsInstance(f("a + (2 + 3)").right, tree.Num)
        self.assertIsInstance(f("a + b"), tree.Binary)


class CodegenTests(unittest.TestCase):
    def asm(self, text):
        return compile_to_asm(text)

    def test_constant_expression_is_folded_to_one_load(self):
        a = self.asm("x = 2 + 3 * 4")
        self.assertIn("LD   V0, 14", a)
        self.assertNotIn("ADD", a)

    def test_increment_uses_add_immediate(self):
        a = self.asm("x = 1\nx = x + 1\nx = x - 1")
        self.assertIn("ADD  V0, 1", a)
        self.assertIn("ADD  V0, 255", a)     # x - 1 is x + 255 (mod 256)

    def test_print_routine_only_emitted_when_used(self):
        self.assertNotIn("_print:", self.asm("x = 1"))
        self.assertIn("_print:", self.asm("print(1)"))

    def test_output_assembles(self):
        rom = compile_to_rom("x = 1\nwhile x < 5 { x = x + 1 }\nprint(x)")
        self.assertEqual(rom[0] >> 4, 0x6)   # starts with LD Vx, byte at 0x200
        self.assertLess(len(rom), 200)

    def test_too_many_variables(self):
        names = [f"v{i}" for i in range(13)]
        src = "\n".join(f"{n} = {i}" for i, n in enumerate(names))
        self.assertIn("too many variables: 13", str(error_for(src)))
        compile_to_asm("\n".join(f"{n} = {i}" for i, n in enumerate(names[:12])))

    def test_expression_too_deep_for_registers(self):
        # 11 variables leave one temporary; a nested right operand needs more.
        src = "\n".join(f"v{i} = {i}" for i in range(11))
        src += "\nr = v0 + (v1 + (v2 + (v3 + 1)))\n"
        # r makes it 12 variables, so no temporaries are free
        e = error_for(src)
        self.assertIsNotNone(e)
        self.assertIn("registers", str(e))
        self.assertEqual(e.line, 12)

    def test_self_reference_is_not_clobbered(self):
        # x = y - x must not overwrite x before reading it
        a = self.asm("x = 5\ny = 9\nx = y - x")
        self.assertIn("LD   V0, V2", a)      # result built in a temp, then moved


# ---------------------------------------------------------------- end to end

# The font's digit shapes, 4 pixels wide and 5 tall.
GLYPHS = {
    "0": (0xF, 0x9, 0x9, 0x9, 0xF), "1": (0x2, 0x6, 0x2, 0x2, 0x7),
    "2": (0xF, 0x1, 0xF, 0x8, 0xF), "3": (0xF, 0x1, 0xF, 0x1, 0xF),
    "4": (0x9, 0x9, 0xF, 0x1, 0x1), "5": (0xF, 0x8, 0xF, 0x1, 0xF),
    "6": (0xF, 0x8, 0xF, 0x9, 0xF), "7": (0xF, 0x1, 0x2, 0x4, 0x4),
    "8": (0xF, 0x9, 0xF, 0x9, 0xF), "9": (0xF, 0x9, 0xF, 0x1, 0xF),
}
BY_SHAPE = {v: k for k, v in GLYPHS.items()}


def run_rom(rom, cycles=3_000_000):
    """Run a ROM headless and return the screen as 32 rows of 64 0/1 values."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "p.ch8")
        with open(path, "wb") as f:
            f.write(rom)
        r = subprocess.run([EMULATOR, "--headless", "--cycles", str(cycles), path],
                           capture_output=True, text=True, timeout=60)
    if r.returncode != 0:
        raise AssertionError(f"emulator stopped: {r.stderr.strip()}")
    return [[1 if c == "#" else 0 for c in line.ljust(64)] for line in r.stdout.splitlines()[:32]]


def read_number(screen, x, y):
    """Read the digits drawn starting at pixel (x, y); stops at the first blank."""
    digits = ""
    while x + 4 <= 64 and y + 5 <= 32:
        shape = tuple(sum(screen[y + r][x + c] << (3 - c) for c in range(4)) for r in range(5))
        if shape not in BY_SHAPE:
            break
        digits += BY_SHAPE[shape]
        x += 5
    return int(digits) if digits else None


def run_nibble(source):
    return run_rom(compile_to_rom(source))


@needs_emulator
class EndToEndTests(unittest.TestCase):
    def numbers(self, source, cells):
        """Compile + run, then read the number drawn in each (x, y) cell."""
        screen = run_nibble(source)
        return [read_number(screen, x, y) for x, y in cells]

    def test_arithmetic(self):
        got = self.numbers("""
            at(0, 0)  print(2 + 3 * 4)
            a = 7  b = 5
            at(0, 6)  print(a * b)
            at(0, 12) print(a - b)
            at(0, 18) print(b - a)
            at(0, 24) print((a + b) * 3)
        """, [(0, 0), (0, 6), (0, 12), (0, 18), (0, 24)])
        self.assertEqual(got, [14, 35, 2, 254, 36])

    def test_leading_zeros_are_dropped(self):
        got = self.numbers("""
            at(0, 0)  print(0)
            at(0, 6)  print(7)
            at(0, 12) print(10)
            at(0, 18) print(100)
            at(0, 24) print(255)
        """, [(0, 0), (0, 6), (0, 12), (0, 18), (0, 24)])
        self.assertEqual(got, [0, 7, 10, 100, 255])

    def test_registers_survive_print(self):
        # print borrows V0-V2 internally; variables living there must be intact
        got = self.numbers("""
            a = 11  b = 22  c = 33  d = 44
            at(0, 0)  print(a)
            at(0, 6)  print(b)
            at(0, 12) print(c)
            at(0, 18) print(a + b + c + d)
        """, [(0, 0), (0, 6), (0, 12), (0, 18)])
        self.assertEqual(got, [11, 22, 33, 110])

    def test_loop_and_if(self):
        got = self.numbers("""
            sum = 0  i = 1
            while i <= 10 { sum = sum + i  i = i + 1 }
            at(0, 0) print(sum)
            n = 0
            i = 0
            while i < 20 {
                if i & 1 == 0 { n = n + 1 } else { n = n + 10 }
                i = i + 1
            }
            at(0, 6) print(n)
        """, [(0, 0), (0, 6)])
        self.assertEqual(got, [55, 110])

    def test_comparisons(self):
        cases = [("3 < 5", 1), ("5 < 3", 0), ("3 < 3", 0), ("3 <= 3", 1), ("4 <= 3", 0),
                 ("5 > 3", 1), ("3 > 5", 0), ("3 > 3", 0), ("3 >= 3", 1), ("2 >= 3", 0),
                 ("4 == 4", 1), ("4 == 5", 0), ("4 != 5", 1), ("4 != 4", 0),
                 ("200 > 100", 1), ("100 < 200", 1), ("0 < 255", 1), ("255 < 0", 0)]
        # use variables so nothing is folded away at compile time
        src = "a = 0\nb = 0\n"
        cells = []
        for i, (expr, _) in enumerate(cases):
            left, op, right = expr.split()
            col, row = i % 4, i // 4
            cells.append((col * 16, row * 6))
            src += f"a = {left}\nb = {right}\nat({col * 16}, {row * 6}) print(a {op} b)\n"
        self.assertEqual(self.numbers(src, cells), [want for _, want in cases])

    def test_logic_short_circuits(self):
        # pressed() would be 0 with no keys held; the right side must not run
        # when the left decides. Check values and that the result is 0 or 1.
        got = self.numbers("""
            a = 5  b = 0  c = 9
            at(0, 0)  print(a and c)
            at(0, 6)  print(a and b)
            at(0, 12) print(b or c)
            at(0, 18) print(b or b)
            at(0, 24) print(not b)
            at(16, 0) print(not a)
            at(16, 6) print(a > 3 and c < 10)
            at(16, 12) print(a < 3 or c == 9)
        """, [(0, 0), (0, 6), (0, 12), (0, 18), (0, 24), (16, 0), (16, 6), (16, 12)])
        self.assertEqual(got, [1, 0, 1, 0, 1, 0, 1, 1])

    def test_bitwise_and_wraparound(self):
        got = self.numbers("""
            a = 200  b = 100
            at(0, 0)  print(a + b)
            at(0, 6)  print(a & 15)
            at(0, 12) print(a | 3)
            at(0, 18) print(a ^ b)
            at(0, 24) print(a * b)
        """, [(0, 0), (0, 6), (0, 12), (0, 18), (0, 24)])
        self.assertEqual(got, [44, 8, 203, 172, 32])

    def test_newline_and_clear(self):
        screen = run_nibble("print(12)\nprint(34)\nnewline()\nprint(56)")
        self.assertEqual(read_number(screen, 0, 0), 12)
        self.assertEqual(read_number(screen, 15, 0), 34)   # 2 digits + a space = 15 px
        self.assertEqual(read_number(screen, 0, 6), 56)
        screen = run_nibble("print(12)\nclear()\nprint(99)")
        self.assertEqual(read_number(screen, 0, 0), 99)

    def test_wait_takes_time_but_finishes(self):
        got = self.numbers("wait(3)\nprint(5)", [(0, 0)])
        self.assertEqual(got, [5])

    def test_random_number_is_drawn(self):
        screen = run_nibble("x = rand() & 7\nprint(x)")
        self.assertIn(read_number(screen, 0, 0), range(8))

    @staticmethod
    def layout(values, per_row):
        """Where print() puts each number: left to right, `per_row` to a line."""
        cells, x, y = [], 0, 0
        for i, v in enumerate(values):
            cells.append((x, y))
            x += 5 * len(str(v)) + 5
            if (i + 1) % per_row == 0:
                x, y = 0, y + 6
        return cells

    def example(self, name):
        with open(os.path.join(ROOT, "examples", name), encoding="utf-8") as f:
            return f.read()

    def test_hello_example(self):
        self.assertEqual(self.numbers(self.example("hello.nib"), [(0, 0)]), [14])

    def test_fib_example(self):
        want = [1, 1, 2, 3, 5, 8, 13, 21, 34, 55, 89, 144, 233]
        got = self.numbers(self.example("fib.nib"), self.layout(want, 3))
        self.assertEqual(got, want)

    def test_primes_example(self):
        want = [n for n in range(2, 72) if all(n % d for d in range(2, n))]
        self.assertEqual(len(want), 20)
        got = self.numbers(self.example("primes.nib"), self.layout(want, 4))
        self.assertEqual(got, want)

    def test_counter_example_compiles(self):
        compile_to_rom(self.example("counter.nib"))


def random_program(rng):
    """A random, always-terminating program that prints a batch of values."""
    names = ["a", "b", "c", "d"][: rng.randint(2, 4)]

    def atom():
        return str(rng.choice([0, 1, 2, 3, 7, 10, 100, 127, 128, 200, 255, rng.randint(0, 255)])) \
            if rng.random() < 0.4 else rng.choice(names)

    def expr(depth):
        if depth == 0 or rng.random() < 0.25:
            return atom()
        roll = rng.random()
        if roll < 0.1:
            return f"(not {expr(depth - 1)})"
        op = rng.choice(["+", "-", "*", "&", "|", "^", "==", "!=", "<", "<=", ">", ">=", "and", "or"])
        if op == "*":  # keep loop counts small so programs stay fast
            return f"({expr(depth - 1)} * {rng.randint(0, 9)})"
        return f"({expr(depth - 1)} {op} {expr(depth - 1)})"

    lines = [f"{n} = {rng.randint(0, 255)}" for n in names]

    def statements(n, depth):
        for _ in range(n):
            roll = rng.random()
            if roll < 0.5 or depth == 0:
                lines.append(f"{rng.choice(names)} = {expr(2)}")
            elif roll < 0.8:
                lines.append(f"if {expr(2)} {{")
                statements(rng.randint(1, 2), depth - 1)
                if rng.random() < 0.5:
                    lines.append("} else {")
                    statements(rng.randint(1, 2), depth - 1)
                lines.append("}")
            else:
                lines.append(f"k{depth} = 0")
                lines.append(f"while k{depth} < {rng.randint(1, 5)} {{")
                statements(rng.randint(1, 2), depth - 1)
                lines.append(f"k{depth} = k{depth} + 1")
                lines.append("}")

    statements(rng.randint(2, 6), 2)
    for i in range(rng.randint(4, 12)):
        lines.append(f"at({(i % 4) * 16}, {(i // 4) * 6}) print({expr(3)})")
    return "\n".join(lines)


@needs_emulator
class DifferentialTests(unittest.TestCase):
    """Random programs: the emulator running the compiled ROM must draw exactly
    what the reference interpreter says the program prints."""

    def check_program(self, source):
        expected = nibble_ref.run(source).prints
        try:
            screen = run_nibble(source)
        except NibbleError:
            return  # ran out of registers; reported, not miscompiled
        for x, y, value in expected:
            self.assertEqual(read_number(screen, x, y), value,
                             f"print at ({x},{y}) in:\n{source}")

    def test_random_programs(self):
        for seed in range(120):
            with self.subTest(seed=seed):
                self.check_program(random_program(random.Random(seed)))

    def test_register_pressure_is_reported_not_miscompiled(self):
        # Every shape of expression with 8 variables live and deep nesting.
        base = "".join(f"v{i} = {i * 31 % 251}\n" for i in range(8))
        for expr in ["v0 + v1 * v2 - v3", "(v0 < v1) + (v2 > v3)", "v0 - (v1 - (v2 - v3))",
                     "v7 * (v6 + v5 * (v4 + 1))", "(v0 == v1) or (v2 != v3) and (v4 <= v5)"]:
            with self.subTest(expr=expr):
                self.check_program(base + f"at(0, 0) print({expr})")


if __name__ == "__main__":
    unittest.main()
