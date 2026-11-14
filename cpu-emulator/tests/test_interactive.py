#!/usr/bin/env python3
"""End-to-end tests for the real-time front end.

These run the actual `chip8` program inside a pseudo-terminal, type at it like
a person would, and read back what it draws. They cover what unit tests can't:
keyboard input, real-time timer speed, quitting, and putting the terminal back
the way it was.

Run with `make test-interactive` (builds the program first). Needs Python 3 on
Linux or macOS.
"""

import fcntl
import os
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BIN = os.path.join(ROOT, "chip8")

# Programs are written in assembly and built with the project's own assembler.
sys.path.insert(0, os.path.join(ROOT, "..", "compiler"))
from chip8asm import assemble  # noqa: E402

# Waits for a key, then draws that key's digit at the top-left corner.
ROM_KEY = """
        LD   V1, 0
        LD   V2, 0
        LD   V0, K
        LD   F, V0
        DRW  V1, V2, 5
done:   JP   done
"""

# Counts the delay timer down from 60 (one second at 60 Hz), then draws a "0".
ROM_TIMER = """
        LD   V0, 60
        LD   DT, V0
wait:   LD   V1, DT
        SE   V1, 0
        JP   wait
        LD   V3, 0
        LD   F, V3
        DRW  V4, V5, 5
done:   JP   done
"""

# Starts the sound timer and then idles.
ROM_BEEP = """
        LD   V0, 5
        LD   ST, V0
done:   JP   done
"""

# An opcode the machine can't run.
ROM_BAD = "DW 0xFFFF\n"

# How the first text row of a drawn glyph looks (two pixel rows per text row).
ZERO_TOP = "█▀▀█"  # "0": ####/#..#
FOUR_ROWS = ["█  █", "▀▀▀█", "   ▀"]  # "4"

RESTORE = "\x1b[?25h\x1b[?1049l"  # show cursor, leave the alternate screen


def frame_lines(text):
    """The most recently drawn screen as a list of lines, or None."""
    parts = text.split("\x1b[H")
    if len(parts) < 2:
        return None
    return parts[-1].split("\x1b")[0].split("\r\n")


class Session:
    """The program running in a pseudo-terminal."""

    def __init__(self, source=None, args=(), rows=24, cols=80, rom_path=None):
        """Run a program given as assembly `source`, or an existing `rom_path`."""
        self._tmp = tempfile.TemporaryDirectory()
        rom = rom_path
        if rom is None:
            rom = os.path.join(self._tmp.name, "prog.ch8")
            with open(rom, "wb") as f:
                f.write(assemble(source))
        self.out = bytearray()
        self.exit_code = None
        pid, fd = pty.fork()
        if pid == 0:  # child: give the terminal a size, then become chip8
            try:
                fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
                os.execv(BIN, [BIN, *args, rom])
            finally:
                os._exit(127)
        self.pid, self.fd = pid, fd

    def _read(self, timeout):
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if ready:
            try:
                data = os.read(self.fd, 65536)
            except OSError:  # the child closed its end
                return False
            if not data:
                return False
            self.out += data
        return True

    def text(self):
        return self.out.decode("utf-8", "replace")

    def wait_for(self, predicate, timeout=3.0):
        """Read output until predicate(text) is true. Returns the time, or None."""
        deadline = time.monotonic() + timeout
        while True:
            if predicate(self.text()):
                return time.monotonic()
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            if not self._read(min(remaining, 0.02)):
                return time.monotonic() if predicate(self.text()) else None

    def wait_for_frame(self, timeout=3.0):
        return self.wait_for(lambda t: frame_lines(t) is not None, timeout)

    def settle(self, seconds):
        """Keep reading output for a while."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            self._read(min(0.02, max(0.0, end - time.monotonic())))

    def send(self, data):
        os.write(self.fd, data if isinstance(data, bytes) else data.encode())

    def alive(self):
        done, _ = os.waitpid(self.pid, os.WNOHANG)
        if done:
            raise AssertionError("program exited unexpectedly")
        return True

    def finish(self, timeout=3.0):
        """Wait for the program to exit; returns its exit code."""
        deadline = time.monotonic() + timeout
        while self.exit_code is None:
            self._read(0.02)
            done, status = os.waitpid(self.pid, os.WNOHANG)
            if done:
                self.exit_code = os.waitstatus_to_exitcode(status)
            elif time.monotonic() > deadline:
                raise AssertionError("program did not exit")
        while self._read(0.05):  # collect whatever it printed last
            pass
        return self.exit_code

    def close(self):
        if self.exit_code is None:
            try:
                os.kill(self.pid, 9)
                os.waitpid(self.pid, 0)
            except (ProcessLookupError, ChildProcessError):
                pass
        os.close(self.fd)
        self._tmp.cleanup()


class InteractiveTests(unittest.TestCase):
    def session(self, *args, **kwargs):
        s = Session(*args, **kwargs)
        self.addCleanup(s.close)
        return s

    def test_key_press_reaches_the_program(self):
        s = self.session(ROM_KEY)
        self.assertIsNotNone(s.wait_for_frame(), "no screen was drawn")

        # Nothing is drawn until a key is pressed.
        s.settle(0.3)
        self.assertEqual(frame_lines(s.text())[0].strip(), "")

        s.send("q")  # 'Q' is CHIP-8 key 4, so the program draws a "4"
        ok = s.wait_for(lambda t: (frame_lines(t) or [""])[0].startswith(FOUR_ROWS[0]))
        self.assertIsNotNone(ok, "the key press never showed up on screen")
        lines = frame_lines(s.text())
        for row, expected in enumerate(FOUR_ROWS):
            self.assertTrue(lines[row].startswith(expected), (row, lines[row]))

        s.send("\x1b")
        self.assertEqual(s.finish(), 0)

    def test_picture_is_16_rows_plus_a_status_line(self):
        s = self.session(ROM_KEY)
        s.wait_for_frame()
        s.settle(0.1)
        lines = frame_lines(s.text())
        self.assertEqual(len(lines), 17)
        self.assertTrue(all(len(line) == 64 for line in lines[:16]))
        self.assertIn("Esc quits", lines[16])

    def test_timer_runs_in_real_time(self):
        # 60 timer ticks at 60 Hz should take one second of real time. If the
        # emulator ran flat out, or at the wrong rate, this would be way off.
        s = self.session(ROM_TIMER)
        started = s.wait_for_frame()
        self.assertIsNotNone(started)
        finished = s.wait_for(
            lambda t: (frame_lines(t) or [""])[0].startswith(ZERO_TOP), timeout=4.0
        )
        self.assertIsNotNone(finished, "the program never finished its countdown")
        elapsed = finished - started
        self.assertGreater(elapsed, 0.9, f"too fast: {elapsed:.3f}s")
        self.assertLess(elapsed, 1.4, f"too slow: {elapsed:.3f}s")

    def test_timer_speed_does_not_depend_on_instruction_speed(self):
        # The 60 Hz timer is separate from instructions per second.
        s = self.session(ROM_TIMER, args=["--speed", "2400"])
        started = s.wait_for_frame()
        finished = s.wait_for(
            lambda t: (frame_lines(t) or [""])[0].startswith(ZERO_TOP), timeout=4.0
        )
        self.assertIsNotNone(finished)
        self.assertGreater(finished - started, 0.9)
        self.assertLess(finished - started, 1.4)

    def test_beep_only_with_the_flag(self):
        s = self.session(ROM_BEEP, args=["--beep"])
        self.assertIsNotNone(s.wait_for(lambda t: "\a" in t, timeout=2.0))

        quiet = self.session(ROM_BEEP)
        quiet.wait_for_frame()
        quiet.settle(0.4)
        self.assertNotIn("\a", quiet.text())

    def test_escape_quits_and_restores_the_terminal(self):
        s = self.session(ROM_KEY)
        s.wait_for_frame()
        s.send("\x1b")
        self.assertEqual(s.finish(), 0)
        text = s.text()
        self.assertTrue(text.startswith("\x1b[?1049h\x1b[?25l"))  # entered
        self.assertTrue(text.endswith(RESTORE), repr(text[-40:]))  # and left again

    def test_ctrl_c_quits_and_restores_the_terminal(self):
        s = self.session(ROM_KEY)
        s.wait_for_frame()
        s.send(b"\x03")  # the terminal turns this into SIGINT
        self.assertEqual(s.finish(), 0)
        self.assertTrue(s.text().endswith(RESTORE), repr(s.text()[-40:]))

    def test_arrow_keys_do_not_quit(self):
        s = self.session(ROM_KEY)
        s.wait_for_frame()
        s.send("\x1b[A\x1b[B\x1b[C\x1b[D")
        s.settle(0.4)
        self.assertTrue(s.alive())
        s.send("\x1b")
        self.assertEqual(s.finish(), 0)

    def test_stopped_machine_says_why_and_waits_for_esc(self):
        s = self.session(ROM_BAD)
        ok = s.wait_for(lambda t: "Stopped: can't run opcode FFFF at 0x200" in t)
        self.assertIsNotNone(ok)
        s.settle(0.3)
        self.assertTrue(s.alive())  # still showing the message, not crashed
        s.send("\x1b")
        self.assertEqual(s.finish(), 0)

    def test_terminal_too_small_is_refused(self):
        s = self.session(ROM_KEY, rows=10, cols=30)
        self.assertEqual(s.finish(), 1)
        self.assertIn("terminal too small", s.text())
        self.assertNotIn("\x1b[?1049h", s.text())  # never touched the screen


class HeadlessTests(unittest.TestCase):
    """--headless needs no terminal, so these use plain pipes."""

    def run_headless(self, source, *args):
        with tempfile.TemporaryDirectory() as tmp:
            rom = os.path.join(tmp, "prog.ch8")
            with open(rom, "wb") as f:
                f.write(assemble(source))
            r = subprocess.run(
                [BIN, "--headless", *args, rom], capture_output=True, text=True, timeout=10
            )
        return r

    def test_prints_the_screen(self):
        r = self.run_headless(ROM_TIMER, "--cycles", "2000")
        self.assertEqual(r.returncode, 0)
        rows = r.stdout.splitlines()
        self.assertEqual(rows[0][:4], "####")  # the "0" glyph
        self.assertEqual(rows[1][:4], "#..#")
        self.assertIn("ran 2000 cycles", rows[-1])

    def test_timers_follow_instruction_speed(self):
        # The countdown is 60 timer ticks, which is 60 frames' worth of
        # instructions: about 700 at 700 Hz, about 1400 at 1400 Hz.
        def drawn(*args):
            return self.run_headless(ROM_TIMER, *args).stdout.startswith("####")

        self.assertFalse(drawn("--speed", "700", "--cycles", "600"))
        self.assertTrue(drawn("--speed", "700", "--cycles", "900"))
        self.assertFalse(drawn("--speed", "1400", "--cycles", "900"))
        self.assertTrue(drawn("--speed", "1400", "--cycles", "1700"))

    def test_stopped_machine_exits_nonzero(self):
        r = self.run_headless(ROM_BAD)
        self.assertEqual(r.returncode, 1)
        self.assertIn("can't run opcode FFFF at 0x200", r.stderr)

    def test_usage_errors(self):
        r = subprocess.run([BIN, "--speed", "fast"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("--speed needs a whole number", r.stderr)
        r = subprocess.run([BIN], capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn("no ROM", r.stderr)
        r = subprocess.run([BIN, "--help"], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0)
        self.assertIn("usage: chip8", r.stdout)


if __name__ == "__main__":
    if not os.path.exists(BIN):
        sys.exit(f"{BIN} not found: run `make` first (or `make test-interactive`)")
    unittest.main(verbosity=2)
