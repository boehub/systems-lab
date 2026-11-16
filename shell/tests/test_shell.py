#!/usr/bin/env python3
"""End-to-end tests: run the real shell and check what it does.

    make test          (builds the shell first)

Part 1 runs command lines with `msh -c` / a script and compares output and exit
status. Part 2 drives the shell through a pseudo-terminal to test job control
(Ctrl-C, Ctrl-Z, jobs, fg, bg), which only exists when a terminal is attached.
"""

import os
import pty
import select
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MSH = os.path.join(os.path.dirname(HERE), "msh")

passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"FAIL  {name}  {detail}")


def run(script, cwd=None, env=None, stdin=None):
    r = subprocess.run([MSH, "-c", script], capture_output=True, text=True, timeout=20,
                       cwd=cwd, env=env, input=stdin)
    return r.stdout, r.stderr, r.returncode


def expect(name, script, out=None, status=0, err_has=None, **kw):
    o, e, rc = run(script, **kw)
    ok = (out is None or o == out) and rc == status and (err_has is None or err_has in e)
    check(name, ok, f"\n   out={o!r} err={e!r} status={rc}  (wanted out={out!r} status={status})")


# ---- part 1: command lines ----------------------------------------------------

expect("echo", "echo hello world", "hello world\n")
expect("echo -n", "echo -n a; echo b", "ab\n")
expect("quotes keep spaces", "echo 'a   b' \"c   d\"", "a   b c   d\n")
expect("pipe", "echo hello | tr a-z A-Z", "HELLO\n")
expect("three-stage pipe", "printf 'b\\na\\nc\\n' | sort | head -n 2", "a\nb\n")
expect("exit status is the last command's", "true | false", "", 1)
expect("; runs both", "echo a; echo b", "a\nb\n")
expect("&& stops on failure", "false && echo no; echo yes", "yes\n")
expect("|| runs on failure", "false || echo fallback", "fallback\n")
expect("&& then ||", "true && false || echo rescued", "rescued\n")
expect("skipped && keeps the status for ||", "false && true || echo ok", "ok\n")
expect("$? after success and failure", "true; echo $?; false; echo $?", "0\n1\n")
expect("exit code", "exit 5", status=5)
expect("exit stops the rest", "echo a; exit 3; echo b", "a\n", 3)
expect("command not found", "nosuchprogram_xyz", status=127, err_has="command not found")
expect("not executable", "echo hi > /tmp/msh-test-noexec; /tmp/msh-test-noexec; rm -f /tmp/msh-test-noexec",
       status=0, err_has="Permission denied")

with tempfile.TemporaryDirectory() as d:
    expect("> and <", "echo data > f; cat < f", "data\n", cwd=d)
    expect(">> appends", "echo one > f; echo two >> f; cat f", "one\ntwo\n", cwd=d)
    expect("> truncates", "echo long long > f; echo x > f; cat f", "x\n", cwd=d)
    expect("2> captures errors", "ls /no/such/dir 2> e; cat e | head -c 3", "ls:", cwd=d)
    expect("2>&1 joins streams", "ls /no/such/dir 2>&1 | head -c 3", "ls:", cwd=d)
    expect("redirect to unwritable path", "echo x > /no/such/dir/f", status=1, err_has="No such file")
    expect("redirect a missing input", "cat < nothing", status=1, err_has="nothing")
    with tempfile.TemporaryDirectory() as d2:
        expect("redirection-only creates the file", "> made; ls", "made\n", cwd=d2)
    expect("redirect around a builtin", "echo hi > f; cat f; echo shown", "hi\nshown\n", cwd=d)
    expect("builtin output restored after redirect", "echo a > f; echo b", "b\n", cwd=d)
    with open(os.path.join(d, "script.sh"), "w") as f:
        f.write("#!/path/to/msh\n# a comment\necho from script\nexit 4\n")
    r = subprocess.run([MSH, os.path.join(d, "script.sh")], capture_output=True, text=True)
    check("script file", r.stdout == "from script\n" and r.returncode == 4, repr(r))
    expect("cd and pwd", "cd sub 2>/dev/null; mkdir sub; cd sub; pwd", d + "/sub\n", cwd=d)
    expect("cd -", "mkdir a; cd a; cd - > /dev/null; pwd", d + "\n", cwd=d)
    expect("cd to missing dir", "cd nowhere", status=1, err_has="nowhere")

env = dict(os.environ, GREETING="hi", HOME="/home/test")
expect("$VAR", "echo $GREETING there", "hi there\n", env=env)
expect("${VAR}", "echo ${GREETING}x", "hix\n", env=env)
expect("single quotes block $", "echo '$GREETING'", "$GREETING\n", env=env)
expect("double quotes expand $", 'echo "$GREETING there"', "hi there\n", env=env)
expect("unset var is empty", "echo [$NOPE_XYZ]", "[]\n", env=env)
expect("~", "echo ~ ~/x", "/home/test /home/test/x\n", env=env)
expect("assignment then use", "X=5; echo $X", "5\n")
expect("export reaches children", "export Y=7; sh -c 'echo $Y'", "7\n")
expect("prefix assignment is for one command", "Z=1 sh -c 'echo $Z'; echo [$Z]", "1\n[]\n")
expect("unset", "export Q=1; unset Q; echo [$Q]", "[]\n")
expect("$$ is a pid", "echo $$ | grep -c '^[0-9][0-9]*$'", "1\n")
expect("bad export name", "export 1x=2", status=1, err_has="not a valid name")

expect("syntax error: bad pipe", "echo a |", status=2, err_has="syntax error")
expect("syntax error: quote", "echo 'x", status=2, err_has="unterminated")
expect("syntax error does not run the line", "echo before; echo 'x", "", 2)
expect("comment", "echo shown # hidden", "shown\n")

# pipes carry real data both ways and close properly
expect("pipe ends close (no hang)", "yes | head -n 3", "y\ny\ny\n")
expect("large data through a pipe", "seq 1 20000 | wc -l | tr -d ' '", "20000\n")
expect("pipeline, each stage reads the last", "echo abc | rev | rev", "abc\n")
expect("stdin passes to a child", "cat", "typed\n", stdin="typed\n")

# background jobs
t0 = time.time()
expect("& returns immediately", "sleep 1 >/dev/null 2>&1 & echo started", "started\n")
check("& did not block", time.time() - t0 < 0.9, f"took {time.time() - t0:.2f}s")
t0 = time.time()
expect("wait waits", "sleep 0.5 & wait; echo after", "after\n")
check("wait blocked for the job", time.time() - t0 >= 0.4)
expect("background job's output arrives", "echo bg > /dev/null & wait; echo ok", "ok\n")
expect("jobs lists a running job", "sleep 1 & jobs", "[1]+  Running   sleep 1\n")

# ---- part 2: job control through a terminal --------------------------------------


class Terminal:
    """The shell running on a pseudo-terminal, driven like a person would."""

    def __init__(self):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["PS1"] = ""
            os.execv(MSH, [MSH])
        self.buf = ""
        self.read_until("msh$ ")

    def read_until(self, text, timeout=5.0):
        end = time.time() + timeout
        while text not in self.buf and time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.1)
            if r:
                try:
                    data = os.read(self.fd, 4096)
                except OSError:
                    break
                if not data:
                    break
                self.buf += data.decode(errors="replace")
        ok = text in self.buf
        i = self.buf.find(text) + len(text) if ok else len(self.buf)
        got, self.buf = self.buf[:i], self.buf[i:]
        return got if ok else None

    def send(self, s):
        os.write(self.fd, s.encode())

    def line(self, s, timeout=5.0):
        """Type a command and return its output (up to the next prompt)."""
        self.send(s + "\n")
        return self.read_until("msh$ ", timeout)

    def close(self):
        try:
            os.kill(self.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)
        os.close(self.fd)


def part2():
    t = Terminal()
    try:
        out = t.line("echo interactive")
        check("tty: runs a command", out is not None and "interactive" in out, repr(out))

        # Ctrl-C stops the running program, not the shell
        t.send("sleep 30\n")
        time.sleep(0.4)
        t.send("\x03")
        out = t.read_until("msh$ ", 3)
        check("tty: Ctrl-C kills the foreground job and returns to the prompt", out is not None, repr(out))
        out = t.line("echo $?")
        check("tty: status after Ctrl-C is 130", out is not None and "130" in out, repr(out))

        # Ctrl-C at an idle prompt does nothing to the shell
        t.send("\x03")
        time.sleep(0.2)
        out = t.line("echo still-here")
        check("tty: shell survives Ctrl-C at the prompt", out is not None and "still-here" in out, repr(out))

        # Ctrl-Z stops it, jobs shows it, bg resumes it, fg brings it back
        t.send("sleep 30\n")
        time.sleep(0.4)
        t.send("\x1a")
        out = t.read_until("msh$ ", 3)
        check("tty: Ctrl-Z stops the job", out is not None and "Stopped" in out, repr(out))
        out = t.line("jobs")
        check("tty: jobs shows it stopped", out is not None and "Stopped" in out and "sleep 30" in out, repr(out))
        out = t.line("bg")
        check("tty: bg resumes it", out is not None and "sleep 30 &" in out, repr(out))
        out = t.line("jobs")
        check("tty: jobs shows it running", out is not None and "Running" in out, repr(out))
        t.send("fg\n")
        time.sleep(0.4)
        t.send("\x03")
        out = t.read_until("msh$ ", 3)
        check("tty: fg brings it back to the foreground (Ctrl-C ends it)", out is not None, repr(out))
        out = t.line("jobs")
        check("tty: no jobs left", out is not None and "sleep" not in out, repr(out))

        # a finished background job is announced
        t.line("sleep 0.3 &")
        time.sleep(0.8)
        out = t.line("echo next")
        check("tty: finished background job is announced", out is not None and "Done" in out, repr(out))

        # a pipeline stops and resumes as one job
        t.send("sleep 30 | cat\n")
        time.sleep(0.4)
        t.send("\x1a")
        out = t.read_until("msh$ ", 3)
        check("tty: a pipeline stops as one job", out is not None and "Stopped" in out, repr(out))
        t.send("fg\n")
        time.sleep(0.3)
        t.send("\x03")
        out = t.read_until("msh$ ", 3)
        check("tty: a stopped pipeline can be resumed and interrupted", out is not None, repr(out))

        # Ctrl-D at an empty prompt leaves the shell
        t.send("\x04")
        time.sleep(0.4)
        pid, status = os.waitpid(t.pid, os.WNOHANG)
        check("tty: Ctrl-D exits", pid == t.pid, f"{pid} {status}")
        if pid == t.pid:
            t.pid = -1
    finally:
        if t.pid > 0:
            t.close()
        else:
            os.close(t.fd)


part2()

print(f"{passed} checks passed, {failed} failed")
sys.exit(1 if failed else 0)
