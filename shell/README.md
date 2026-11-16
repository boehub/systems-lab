# msh: a small Unix shell

A shell written in C from the system calls up (`fork`, `execvp`, `pipe`, `dup2`, `waitpid`, process groups). It runs real shell scripts (`if`, loops, `case`, functions, here-documents, `$(...)`) and does job control for real.

```
make          # builds ./msh
./msh         # interactive
./msh script.sh arg1 arg2     # the words become $1, $2, ...
./msh -c 'echo hi | tr a-z A-Z; echo status $?'
./msh examples/wordfreq.sh README.md     # a real script: word frequencies
```

## What works

| | |
|---|---|
| Pipelines | `a \| b \| c`; the status is the last command's; `! cmd` negates it |
| Lists | `a; b`, `a && b`, `a \|\| b` (left to right, like bash), and a line may end in `\|`, `&&` or `\|\|` to continue |
| Redirection | `< file`, `> file`, `>> file`, `2> file`, `2>&1`, `>&2`, `2>> file`, in the order written; also on loops and groups (`done < file`) |
| Here-documents | `<<EOF`, `<<'EOF'` (no expansion), `<<-EOF` (strips leading tabs); several on one line |
| Quoting | `'literal'`, `"with $vars"`, backslash escapes, backslash-newline continues a line |
| Expansion | `$NAME`, `${NAME}`, `${NAME:-default}`, `${#NAME}`, `$?`, `$$`, `$#`, `$@`, `$*`, `$1`..., `$(cmd)`, `` `cmd` ``, `$((1 + 2 * x))`, `~`; unquoted results are split into words; `*`, `?`, `[...]` match files |
| Control flow | `if`/`elif`/`else`/`fi`, `while`, `until`, `for x in ...` (or over `"$@"`), `case` with `\|` patterns, `break N`, `continue N` |
| Grouping | `( ... )` runs in a subshell (its variables and `cd` do not leak), `{ ...; }` runs in the shell; both can sit in pipelines |
| Functions | `name() { ...; }` or `function name { ...; }`, with `$1`..., `$#`, `$@`, `return N` |
| Variables | `NAME=value` alone, or as a prefix for one command; `x=$(false)` keeps the failing status |
| Built-ins | `cd` (`cd -` too), `pwd`, `echo` (`-n`), `exit`, `export`, `unset`, `true`, `false`, `:`, `test` / `[` (files, strings, numbers, `!`, `-a`, `-o`, parentheses), `read`, `shift`, `set --`, `source` / `.`, `eval`, `jobs`, `fg`, `bg`, `wait` |
| Background | `cmd &`, with a finished-job notice at the next prompt |
| Job control | Ctrl-C ends the running job only, Ctrl-Z stops it, `jobs` lists, `bg` resumes in the background, `fg` brings it back. Pipelines stop and resume as one job |
| Exit codes | 127 not found, 126 not executable, 128+N killed by signal N, 2 for a syntax error |

## How it is put together

- `src/parse.c`: **lexer** (text to tokens, quote-aware, reads here-document bodies), a recursive-descent **parser** (tokens to a tree: lists of pipelines of commands, where a command is simple or one of `if`/`while`/`for`/`case`/`( )`/`{ }`/function), and **expansion** (a raw word to the words the program gets). It never runs anything, so it is tested on its own (`tests/test_parse.c`, 142 checks, including a 60,000-input fuzz run under the sanitizers).
- `src/msh.c`: **execution.** A pipeline is a chain of `fork`s joined by `pipe`s; each child sets up its redirections with `dup2` and calls `execvp`. A lone built-in runs inside the shell itself (otherwise `cd` would only change the directory of a child that immediately exits), with the standard streams saved and restored around it.
- **Job control** follows the textbook design: every pipeline gets its own process group, the shell hands the terminal to the foreground group with `tcsetpgrp` and takes it back afterwards, and waits with `WUNTRACED` so a Ctrl-Z shows up as a stop rather than a hang. The shell itself ignores `SIGINT`, `SIGTSTP` and the terminal-access signals so that only the job receives them. Children put those back to the defaults before `exec`.
- **Globbing** (`*`, `?`, `[...]`) uses `glob(3)`. The parser marks which pattern characters were typed bare and which came from quotes, backslashes or `$VARIABLES`, and escapes the latter so only typed wildcards match. A pattern that matches nothing is left as typed, as in bash. Redirection targets are not globbed.
- **Incomplete input.** The parser tells "this is wrong" apart from "this is not finished yet" (`if` without `fi`, an open quote, a trailing `|`, a here-document with no end marker). A terminal answers the second with a `> ` prompt and reads another line; a script keeps reading until the command is complete, and an error is only reported if the file ends first.
- **Command substitution** forks a child shell that runs the text with its output on a pipe; the parent reads it, strips trailing newlines, and records the status. Unquoted results are split on white space; quoted ones stay whole. `"$@"` yields one word per parameter. Text that came from a variable or substitution is never treated as a wildcard.
- **Compound commands in pipelines.** A loop or group on its own runs inside the shell; in a pipeline, in the background, or in `( )` it runs in a forked child, so `for ...; done | sort` and `( cd /tmp; ... ) &` work. Ctrl-C at the terminal stops a running loop, not just the command inside it.
- **Functions** are stored as source text and parsed again on each call, which keeps their lifetime simple.
- Variables are expanded *when a command runs*, not when the line is read, so `false; echo $?` sees the status of `false`.

## Tests

```
make test       # parser unit tests, then the real shell end to end
make sanitize   # the same, built with AddressSanitizer + UBSan
```

`tests/test_shell.py` runs 172 checks: command lines compared with expected output and exit status (pipes, redirects, quoting, variables, errors, a 20,000-line pipe, background jobs), then a second part that runs the shell on a **pseudo-terminal** and types Ctrl-C, Ctrl-Z, `bg`, `fg` and Ctrl-D the way a person would. The sanitizer run found a real memory leak in the parser's error path while this was being built.

## What it deliberately leaves out

This is a learning project, not a bash replacement.

- No `[[ ]]`, no arrays, no `local` (every variable is global), no aliases, no `trap`, no `set -e`, no `getopts`, no `printf` built-in (the system one is used), no `$(( ))` bit operators or assignment, and `${...}` supports only `:-` and `#`.
- No line editing or history: the interactive prompt reads lines with plain `getline`.
- A `case` pattern list inside `$( )` can confuse the matching of the closing parenthesis.
- Wildcard characters that come from a `$VARIABLE` or `$(...)` are not expanded (bash would expand them).
- `a && b &` puts only `b` in the background (bash backgrounds the whole list).
- Every variable is an environment variable, so `x=1` is visible to the programs it starts (there is no separate "shell-only" variable).
- Stopped-pipeline bookkeeping assumes the stop signal reaches every member of the group, which is what a Ctrl-Z does.
