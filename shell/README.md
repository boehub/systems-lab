# msh: a small Unix shell

A shell written in C from the system calls up (`fork`, `execvp`, `pipe`, `dup2`, `waitpid`, process groups). It does what you expect from the first few chapters of a shell manual, and it does job control for real.

```
make          # builds ./msh
./msh         # interactive
./msh script.sh
./msh -c 'echo hi | tr a-z A-Z; echo status $?'
```

## What works

| | |
|---|---|
| Pipelines | `a \| b \| c`; the status is the last command's |
| Lists | `a; b`, `a && b`, `a \|\| b` (left to right, like bash) |
| Redirection | `< file`, `> file`, `>> file`, `2> file`, `2>&1`, in the order written |
| Quoting | `'literal'`, `"with $vars"`, backslash escapes |
| Variables | `$NAME`, `${NAME}`, `$?`, `$$`, `~`; `NAME=value` alone, or as a prefix for one command |
| Built-ins | `cd` (`cd -` too), `pwd`, `echo` (`-n`), `exit`, `export`, `unset`, `true`, `false`, `:`, `jobs`, `fg`, `bg`, `wait` |
| Background | `cmd &`, with a finished-job notice at the next prompt |
| Job control | Ctrl-C ends the running job only, Ctrl-Z stops it, `jobs` lists, `bg` resumes in the background, `fg` brings it back. Pipelines stop and resume as one job |
| Exit codes | 127 not found, 126 not executable, 128+N killed by signal N, 2 for a syntax error |

## How it is put together

- `src/parse.c`: **lexer** (text to tokens, quote-aware), **parser** (tokens to a list of pipelines of commands), and **expansion** (a raw word to the text the program gets). It never runs anything, so it is tested on its own (`tests/test_parse.c`, 57 checks).
- `src/msh.c`: **execution.** A pipeline is a chain of `fork`s joined by `pipe`s; each child sets up its redirections with `dup2` and calls `execvp`. A lone built-in runs inside the shell itself (otherwise `cd` would only change the directory of a child that immediately exits), with the standard streams saved and restored around it.
- **Job control** follows the textbook design: every pipeline gets its own process group, the shell hands the terminal to the foreground group with `tcsetpgrp` and takes it back afterwards, and waits with `WUNTRACED` so a Ctrl-Z shows up as a stop rather than a hang. The shell itself ignores `SIGINT`, `SIGTSTP` and the terminal-access signals so that only the job receives them. Children put those back to the defaults before `exec`.
- Variables are expanded *when a command runs*, not when the line is read, so `false; echo $?` sees the status of `false`.

## Tests

```
make test       # parser unit tests, then the real shell end to end
make sanitize   # the same, built with AddressSanitizer + UBSan
```

`tests/test_shell.py` runs 70 checks: command lines compared with expected output and exit status (pipes, redirects, quoting, variables, errors, a 20,000-line pipe, background jobs), then a second part that runs the shell on a **pseudo-terminal** and types Ctrl-C, Ctrl-Z, `bg`, `fg` and Ctrl-D the way a person would. The sanitizer run found a real memory leak in the parser's error path while this was being built.

## What it deliberately leaves out

This is a learning project, not a bash replacement.

- No globbing (`*.c`), no word splitting (a variable holding `a b` is one argument), no `$(...)` or backticks, no here-documents, no subshells `( )`, no `if` / `for` / functions, no aliases.
- No line editing or history: the interactive prompt reads whole lines with plain `getline`.
- A script is run one line at a time, so a command cannot span lines.
- `a && b &` puts only `b` in the background (bash backgrounds the whole list).
- Every variable is an environment variable (there is no separate "shell-only" variable).
- Stopped-pipeline bookkeeping assumes the stop signal reaches every member of the group, which is what a Ctrl-Z does.
