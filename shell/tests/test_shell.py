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

_g = tempfile.mkdtemp()
for _n in ("a.c", "b.c", "c.h", "we ird.c", ".hidden.c"):
    open(os.path.join(_g, _n), "w").close()
expect("glob expands, sorted", "echo *.c", "a.c b.c we ird.c\n", cwd=_g)
expect("glob skips dotfiles", "echo *", "a.c b.c c.h we ird.c\n", cwd=_g)
expect("glob with ? and []", "echo ?.[ch]", "a.c b.c c.h\n", cwd=_g)
expect("glob keeps names with spaces whole", "ls *ird.c | wc -l; for_each=1", "1\n", cwd=_g)
expect("quoted * is literal", "echo '*.c' \"*.c\" \\*.c", "*.c *.c *.c\n", cwd=_g)
expect("no match leaves the word", "echo *.zz", "*.zz\n", cwd=_g)
expect("glob in a directory path", "mkdir sub; touch sub/x.txt; echo s*/*.txt", "sub/x.txt\n", cwd=_g)
expect("$VAR holding * is not a pattern", "x='*.c'; echo $x", "*.c\n", cwd=_g)
expect("glob not applied to redirect names", "echo hi > *.out; ls", "*.out\na.c\nb.c\nc.h\nsub\nwe ird.c\n", cwd=_g)

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

# ---- compound commands, substitution, here-documents, functions ---------------

expect(">&2 sends stdout to stderr", 'echo to-err >&2 2>/dev/null; echo out', "out\n")
expect("1>&2 is the same", 'echo e 2> e.txt 1>&2; cat e.txt', "e\n", cwd=_g)
expect("2>> appends", 'echo a 2>>e2.txt >&2; echo b 2>>e2.txt >&2; cat e2.txt', "a\nb\n", cwd=_g)
expect("error text can be captured by 2>&1", 'ls /nonexistent-xyz 2>&1 | wc -l', "1\n")
expect("if / else", 'if true; then echo yes; else echo no; fi', "yes\n")
expect("if takes the else branch", 'if false; then echo yes; else echo no; fi', "no\n")
expect("elif", 'x=2; if [ $x = 1 ]; then echo one; elif [ $x = 2 ]; then echo two; else echo other; fi', "two\n")
expect("if with no else and a false test is quiet, status 0", 'if false; then echo no; fi', "", 0)
expect("if spread over lines", "if true\nthen\n  echo multi\nfi\n", "multi\n")
expect("for over a list", 'for i in a b c; do echo $i; done', "a\nb\nc\n")
expect("for over a glob", "for f in ?.c; do echo $f; done", "a.c\nb.c\n", cwd=_g)
expect("for with no `in` uses the arguments", 'set -- x "y z"; for a; do echo "[$a]"; done', "[x]\n[y z]\n")
expect("while", 'n=0; while [ $n -lt 3 ]; do echo $n; n=$((n+1)); done', "0\n1\n2\n")
expect("until", 'n=3; until [ $n -le 0 ]; do n=$((n-1)); done; echo $n', "0\n")
expect("break", 'for i in 1 2 3; do if [ $i = 2 ]; then break; fi; echo $i; done', "1\n")
expect("continue", 'for i in 1 2 3; do if [ $i = 2 ]; then continue; fi; echo $i; done', "1\n3\n")
expect("break 2 leaves both loops", 'for i in 1 2; do for j in a b; do echo $i$j; break 2; done; done; echo end', "1a\nend\n")
expect("continue 2 continues the outer loop", 'for i in 1 2; do for j in a b; do echo $i$j; continue 2; done; done', "1a\n2a\n")
expect("loop output can be piped", 'for i in 3 1 2; do echo $i; done | sort', "1\n2\n3\n")
expect("loop input can be redirected", 'printf "a\\nb\\n" > f.txt; while read l; do echo "<$l>"; done < f.txt', "<a>\n<b>\n", cwd=_g)
expect("case matches a pattern", 'case hello in h*) echo h;; *) echo other;; esac', "h\n")
expect("case alternatives and default", 'case b in a|b) echo ab;; *) echo no;; esac', "ab\n")
expect("case falls to *", 'case z in a) echo a;; *) echo star;; esac', "star\n")
expect("case with a quoted pattern is literal", 'case "*" in "*") echo star;; *) echo no;; esac', "star\n")
expect("subshell does not leak variables", 'a=1; (a=2; echo $a); echo $a', "2\n1\n")
expect("subshell does not change directory", 'cd /; (cd /tmp); pwd', "/\n")
expect("subshell exit status", '(exit 3); echo $?', "3\n")
expect("group runs in the shell", '{ a=5; }; echo $a', "5\n")
expect("group can be piped", '{ echo a; echo b; } | tr a-z A-Z', "A\nB\n")
expect("negation", '! false && echo ok; ! true; echo $?', "ok\n1\n")
expect("command substitution", 'echo "[$(echo hi)]"', "[hi]\n")
expect("backquotes", 'echo `echo hi`', "hi\n")
expect("substitution strips trailing newlines only", 'x=$(printf "a\\n\\n\\nb\\n\\n"); echo "$x"', "a\n\n\nb\n")
expect("nested substitution", 'echo $(echo $(echo deep))', "deep\n")
expect("substitution with quotes inside", 'echo "$(echo "a b")"', "a b\n")
expect("substitution status shows in $?", 'x=$(false); echo $?; x=$(true); echo $?', "1\n0\n")
expect("unquoted substitution splits words", 'for w in $(echo a b   c); do echo "[$w]"; done', "[a]\n[b]\n[c]\n")
expect("quoted substitution does not split", 'for w in "$(echo a b c)"; do echo "[$w]"; done', "[a b c]\n")
expect("unquoted variable splits", 'x="a  b"; for w in $x; do echo "[$w]"; done', "[a]\n[b]\n")
expect("quoted variable does not", 'x="a  b"; for w in "$x"; do echo "[$w]"; done', "[a  b]\n")
expect("an empty unquoted variable vanishes", 'x=; set -- $x end; echo $#', "1\n")
expect("an empty quoted variable stays", 'x=; set -- "$x" end; echo $#', "2\n")
expect("assignment is not split", 'x=$(echo a b); echo "$x"', "a b\n")
expect("arithmetic", 'echo $((1 + 2 * 3)) $(( (1+2)*3 )) $((10 % 4)) $((7/2)) $((-3 + 1))', "7 9 2 3 -2\n")
expect("arithmetic with variables", 'x=4; y=3; echo $((x*y + 1)) $((x > y)) $((x == y))', "13 1 0\n")
expect("arithmetic division by zero", 'echo $((1/0))', "0\n", err_has="division by zero")
expect("${var:-default}", 'echo ${nope:-fallback} ${HOME:-no}', f"fallback {os.environ.get('HOME', '')}\n")
expect("${#var}", 'x=hello; echo ${#x}', "5\n")
expect("here-document", 'cat <<EOF\nline 1\nline 2\nEOF\n', "line 1\nline 2\n")
expect("here-document expands variables", 'x=world; cat <<EOF\nhello $x\nEOF\n', "hello world\n")
expect("here-document with a quoted marker is literal", "x=world; cat <<'EOF'\nhello $x\nEOF\n", "hello $x\n")
expect("here-document with <<- strips tabs", "cat <<-EOF\n\tindented\n\tEOF\n", "indented\n")
expect("here-document feeds a pipeline", 'cat <<EOF | tr a-z A-Z\nabc\nEOF\n', "ABC\n")
expect("here-document then more commands", 'cat <<EOF\nx\nEOF\necho after\n', "x\nafter\n")
expect("two here-documents on one line", 'cat <<A; cat <<B\nfirst\nA\nsecond\nB\n', "first\nsecond\n")
expect("function with arguments", 'f() { echo "$1-$2 ($#)"; }; f a b c', "a-b (3)\n")
expect("function return status", 'f() { return 4; }; f; echo $?', "4\n")
expect("function can use $@", 'f() { for a in "$@"; do echo "[$a]"; done; }; f "x y" z', "[x y]\n[z]\n")
expect("function arguments are restored afterwards", 'set -- a b; f() { echo $1; }; f z; echo $1', "z\na\n")
expect("function defined with the keyword", 'function g { echo in-g; }; g', "in-g\n")
expect("functions can recurse", 'f() { if [ $1 -le 0 ]; then echo done; else f $(($1-1)); fi; }; f 3', "done\n")
expect("function in a pipeline", 'f() { tr a-z A-Z; }; echo abc | f', "ABC\n")
expect("exit inside a function leaves the shell", 'f() { exit 9; }; f; echo not-reached', "", 9)
expect("test: string", '[ abc = abc ] && echo eq; [ abc != abd ] && echo ne; [ -z "" ] && echo z; [ -n x ] && echo n', "eq\nne\nz\nn\n")
expect("test: numbers", '[ 2 -lt 10 ] && echo lt; [ 10 -ge 10 ] && echo ge; [ 3 -eq 4 ] || echo noteq', "lt\nge\nnoteq\n")
expect("test: files", 'touch tf; [ -e tf ] && echo e; [ -f tf ] && echo f; [ -d . ] && echo d; [ -d tf ] || echo notd; [ -s tf ] || echo empty', "e\nf\nd\nnotd\nempty\n", cwd=_g)
expect("test: not, and, or", '[ ! -e nothing ] && echo a; [ 1 = 1 -a 2 = 2 ] && echo b; [ 1 = 2 -o 2 = 2 ] && echo c', "a\nb\nc\n")
expect("test: missing bracket", '[ a = a', "", 2, err_has="missing ]")
expect("test: bad number", '[ a -lt 3 ]', "", 2, err_has="bad expression")
expect("read splits into variables", 'echo "a b c d" | { read x y; echo "$x|$y"; }', "a|b c d\n")
expect("read at end of input fails", 'read x < /dev/null; echo $?', "1\n")
expect("eval", 'cmd="echo from eval"; eval $cmd', "from eval\n")
expect("source", 'echo "v=sourced" > s.sh; . ./s.sh; echo $v', "sourced\n", cwd=_g)
expect("shift", 'set -- a b c; shift; echo $1 $#; shift 2; echo $#', "b 2\n0\n")
expect("comments and blank lines in a script", "# hi\n\necho a # trailing\n\n# bye\n", "a\n")
expect("line continuation", "echo one \\\ntwo\n", "one two\n")
expect("&& at the end of a line continues", "true &&\necho continued\n", "continued\n")
expect("pipe at the end of a line continues", "echo abc |\ntr a-z A-Z\n", "ABC\n")
expect("syntax error: missing fi", 'if true; then echo a', "", 2, err_has="missing `fi'")
expect("syntax error: stray fi", 'fi', "", 2, err_has="unexpected token fi")
expect("syntax error: missing done", 'for i in 1 2; do echo $i', "", 2, err_has="missing `done'")
expect("syntax error: unterminated here-document", 'cat <<EOF\nx\n', "", 2, err_has="here-document")
expect("syntax error: unterminated substitution", 'echo $(echo', "", 2, err_has="command substitution")
expect("syntax error: bad function body", 'f() echo hi', "", 2, err_has="function body")
expect("syntax error stops the whole script", 'echo before\nif true; then\necho a\n', "", 2)

# the example script
_ex = os.path.join(os.path.dirname(HERE), "examples", "wordfreq.sh")
_doc = os.path.join(_g, "words.txt")
open(_doc, "w").write("b a b c b a\n")
o = subprocess.run([MSH, _ex, _doc, "/no/such/file"], capture_output=True, text=True)
check("example script: counts and top words", o.stdout == f"{_doc}: 6 words\ntotal: 6 words\n\n3 b\n2 a\n1 c\n" and "not a file" in o.stderr, repr(o.stdout + o.stderr))
o = subprocess.run([MSH, _ex], capture_output=True, text=True)
check("example script: usage and status 2", o.returncode == 2 and "usage:" in o.stdout, repr(o.stdout))

# the script file form, with arguments
_script = os.path.join(_g, "args.sh")
open(_script, "w").write('echo "$0:$#:$1:$2"\nfor a in "$@"; do echo "[$a]"; done\n')
o, e, rc = subprocess.run([MSH, _script, "p", "q r"], capture_output=True, text=True).stdout, "", 0
check("script arguments", o == f"{_script}:2:p:q r\n[p]\n[q r]\n", repr(o))

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

        # a command typed over several lines: the shell asks for more with a "> " prompt
        t.send("for i in 1 2; do\n")
        out = t.read_until("> ", 3)
        check("tty: an unfinished for loop gets the continuation prompt", out is not None, repr(out))
        t.send("echo item$i\n")
        t.read_until("> ", 3)
        t.send("done\n")
        out = t.read_until("msh$ ", 3)
        check("tty: the loop runs once it is finished", out is not None and "item1" in out and "item2" in out, repr(out))
        t.send("cat <<EOF\n")
        t.read_until("> ", 3)
        t.send("typed text\n")
        t.read_until("> ", 3)
        t.send("EOF\n")
        out = t.read_until("msh$ ", 3)
        check("tty: a here-document can be typed", out is not None and "typed text" in out, repr(out))

        # Ctrl-C ends a running loop, not just the command inside it
        t.send("while true; do sleep 1; echo tick; done\n")
        time.sleep(1.5)
        t.send("\x03")
        out = t.read_until("msh$ ", 3)
        check("tty: Ctrl-C stops a loop and returns to the prompt", out is not None, repr(out))
        time.sleep(1.3)
        out = t.line("echo quiet")
        check("tty: the loop really is gone", out is not None and "tick" not in out.split("quiet")[0].split("echo quiet")[-1], repr(out))

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
