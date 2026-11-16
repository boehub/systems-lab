// msh: a small Unix shell.
//
//   msh                 interactive (reads from the terminal)
//   msh script.sh       run a file, one line at a time
//   msh -c 'cmd; cmd'   run a string
//
// Pipes, redirection, && || ; &, quoting, $VARIABLES, a few built-ins, and
// job control (Ctrl-C, Ctrl-Z, jobs, fg, bg). See README.md for what is left out.

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "parse.h"

#define MAX_PIPE 64
#define MAX_JOBS 32

// ---- shell state ------------------------------------------------------------

static int interactive;     // reading from a terminal: job control and prompts
static pid_t shell_pgid;
static int last_status;     // $?
static int want_exit;       // set by the `exit` built-in
static int exit_code;

typedef enum { J_FREE, J_RUNNING, J_STOPPED } JobState;

typedef struct {
    JobState state;
    int id;
    pid_t pgid;
    int n;
    pid_t pids[MAX_PIPE];
    char done[MAX_PIPE];
    int status;  // exit status of the last process in the pipeline
    int bg;
    char text[200];
} Job;

static Job jobs[MAX_JOBS];

static const char *lookup_var(const char *name, void *ctx) {
    static char buf[32];
    (void)ctx;
    if (!strcmp(name, "?")) { snprintf(buf, sizeof buf, "%d", last_status); return buf; }
    if (!strcmp(name, "$")) { snprintf(buf, sizeof buf, "%d", (int)getpid()); return buf; }
    return getenv(name);  // every variable lives in the environment
}

static char *expand(const char *raw) { return expand_word(raw, lookup_var, NULL); }

// ---- jobs -------------------------------------------------------------------

static int decode(int st) {
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
    return 1;
}

static Job *new_job(const char *text) {
    int max_id = 0;
    for (int i = 0; i < MAX_JOBS; ++i)
        if (jobs[i].state != J_FREE && jobs[i].id > max_id) max_id = jobs[i].id;
    for (int i = 0; i < MAX_JOBS; ++i) {
        if (jobs[i].state == J_FREE) {
            memset(&jobs[i], 0, sizeof jobs[i]);
            jobs[i].state = J_RUNNING;
            jobs[i].id = max_id + 1;
            snprintf(jobs[i].text, sizeof jobs[i].text, "%s", text);
            return &jobs[i];
        }
    }
    return NULL;
}

static Job *find_job(const char *spec) {
    if (spec && *spec == '%') spec++;
    Job *best = NULL;
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state == J_FREE) continue;
        if (spec && *spec) {
            if (j->id == atoi(spec)) return j;
        } else if (!best || j->id > best->id) {
            best = j;
        }
    }
    return spec && *spec ? NULL : best;
}

// Wait for a job to finish or stop. Returns 1 if it stopped.
static int wait_job(Job *j) {
    for (int i = 0; i < j->n; ++i) {
        if (j->done[i]) continue;
        int st;
        pid_t r;
        do r = waitpid(j->pids[i], &st, WUNTRACED); while (r < 0 && errno == EINTR);
        if (r < 0) { j->done[i] = 1; continue; }
        if (WIFSTOPPED(st)) {
            // The whole group got the stop signal; consume the other reports now
            // so they aren't mistaken for new stops after the job is resumed.
            for (int k = i + 1; k < j->n; ++k) {
                int st2;
                if (!j->done[k] && waitpid(j->pids[k], &st2, WNOHANG | WUNTRACED) == j->pids[k] &&
                    !WIFSTOPPED(st2)) {
                    j->done[k] = 1;
                    if (k == j->n - 1) j->status = decode(st2);
                }
            }
            j->state = J_STOPPED;
            return 1;
        }
        j->done[i] = 1;
        if (i == j->n - 1) {
            j->status = decode(st);
            if (interactive && WIFSIGNALED(st) && WTERMSIG(st) != SIGINT && WTERMSIG(st) != SIGPIPE)
                fprintf(stderr, "%s\n", strsignal(WTERMSIG(st)));
        }
    }
    return 0;
}

// Run a job in the foreground until it finishes or stops.
static int run_foreground(Job *j) {
    j->bg = 0;
    j->state = J_RUNNING;
    if (interactive) tcsetpgrp(STDIN_FILENO, j->pgid);
    int stopped = wait_job(j);
    if (interactive) tcsetpgrp(STDIN_FILENO, shell_pgid);
    if (stopped) {
        fprintf(stderr, "\n[%d]+  Stopped                 %s\n", j->id, j->text);
        return 128 + SIGTSTP;
    }
    int status = j->status;
    j->state = J_FREE;
    return status;
}

static void signal_job(Job *j, int sig) {
    if (interactive) {
        kill(-j->pgid, sig);
    } else {
        for (int i = 0; i < j->n; ++i)
            if (!j->done[i]) kill(j->pids[i], sig);
    }
}

// Collect finished background jobs; say so when running interactively.
static void reap_jobs(int announce) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state != J_RUNNING) continue;
        int alive = 0;
        for (int k = 0; k < j->n; ++k) {
            if (j->done[k]) continue;
            int st;
            pid_t r = waitpid(j->pids[k], &st, WNOHANG | WUNTRACED);
            if (r == 0) { alive++; continue; }
            if (r > 0 && WIFSTOPPED(st)) { j->state = J_STOPPED; alive++; continue; }
            j->done[k] = 1;
            if (r > 0 && k == j->n - 1) j->status = decode(st);
        }
        if (alive == 0 && j->state == J_RUNNING) {
            if (announce && interactive)
                fprintf(stderr, "[%d]+  Done                    %s\n", j->id, j->text);
            j->state = J_FREE;
        }
    }
}

// ---- built-ins --------------------------------------------------------------

static const char *const kBuiltins[] = {"cd", "pwd", "echo", "exit", "export", "unset", "true",
                                        "false", ":", "jobs", "fg", "bg", "wait", NULL};

static int is_builtin(const char *name) {
    for (int i = 0; kBuiltins[i]; ++i)
        if (!strcmp(name, kBuiltins[i])) return 1;
    return 0;
}

static int bi_cd(char **argv) {
    const char *dir = argv[1];
    int print = 0;
    if (!dir) {
        dir = getenv("HOME");
        if (!dir) { fprintf(stderr, "msh: cd: HOME not set\n"); return 1; }
    } else if (!strcmp(dir, "-")) {
        dir = getenv("OLDPWD");
        if (!dir) { fprintf(stderr, "msh: cd: OLDPWD not set\n"); return 1; }
        print = 1;
    }
    char old[4096];
    if (!getcwd(old, sizeof old)) old[0] = '\0';
    if (chdir(dir) != 0) {
        fprintf(stderr, "msh: cd: %s: %s\n", dir, strerror(errno));
        return 1;
    }
    char now[4096];
    if (getcwd(now, sizeof now)) {
        setenv("PWD", now, 1);
        if (print) printf("%s\n", now);
    }
    if (old[0]) setenv("OLDPWD", old, 1);
    return 0;
}

static int bi_echo(char **argv) {
    int i = 1, newline = 1;
    if (argv[i] && !strcmp(argv[i], "-n")) { newline = 0; i++; }
    for (; argv[i]; ++i) {
        fputs(argv[i], stdout);
        if (argv[i + 1]) fputc(' ', stdout);
    }
    if (newline) fputc('\n', stdout);
    fflush(stdout);
    return 0;
}

static int valid_name(const char *s, size_t len) {
    if (len == 0 || (s[0] >= '0' && s[0] <= '9')) return 0;
    for (size_t i = 0; i < len; ++i) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'))
            return 0;
    }
    return 1;
}

static int bi_export(char **argv) {
    int status = 0;
    for (int i = 1; argv[i]; ++i) {
        const char *eq = strchr(argv[i], '=');
        size_t len = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if (!valid_name(argv[i], len)) {
            fprintf(stderr, "msh: export: '%s': not a valid name\n", argv[i]);
            status = 1;
        } else if (eq) {
            char *name = strndup(argv[i], len);
            setenv(name, eq + 1, 1);
            free(name);
        }
    }
    return status;
}

static int bi_jobs(void) {
    reap_jobs(0);
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state == J_FREE) continue;
        printf("[%d]+  %-8s  %s\n", j->id, j->state == J_STOPPED ? "Stopped" : "Running", j->text);
    }
    fflush(stdout);
    return 0;
}

static int bi_fg(char **argv) {
    Job *j = find_job(argv[1]);
    if (!j) { fprintf(stderr, "msh: fg: no such job\n"); return 1; }
    fprintf(stderr, "%s\n", j->text);
    signal_job(j, SIGCONT);
    return run_foreground(j);
}

static int bi_bg(char **argv) {
    Job *j = find_job(argv[1]);
    if (!j) { fprintf(stderr, "msh: bg: no such job\n"); return 1; }
    j->state = J_RUNNING;
    j->bg = 1;
    signal_job(j, SIGCONT);
    fprintf(stderr, "[%d]+ %s &\n", j->id, j->text);
    return 0;
}

static int bi_wait(void) {
    for (int i = 0; i < MAX_JOBS; ++i) {
        Job *j = &jobs[i];
        if (j->state == J_RUNNING && j->bg) {
            wait_job(j);
            if (j->state == J_RUNNING) j->state = J_FREE;
        }
    }
    return 0;
}

static int run_builtin(char **argv) {
    const char *n = argv[0];
    if (!strcmp(n, "cd")) return bi_cd(argv);
    if (!strcmp(n, "pwd")) {
        char buf[4096];
        if (!getcwd(buf, sizeof buf)) { perror("msh: pwd"); return 1; }
        printf("%s\n", buf);
        fflush(stdout);
        return 0;
    }
    if (!strcmp(n, "echo")) return bi_echo(argv);
    if (!strcmp(n, "exit")) {
        want_exit = 1;
        exit_code = argv[1] ? atoi(argv[1]) & 0xFF : last_status;
        return exit_code;
    }
    if (!strcmp(n, "export")) return bi_export(argv);
    if (!strcmp(n, "unset")) {
        for (int i = 1; argv[i]; ++i) unsetenv(argv[i]);
        return 0;
    }
    if (!strcmp(n, "true") || !strcmp(n, ":")) return 0;
    if (!strcmp(n, "false")) return 1;
    if (!strcmp(n, "jobs")) return bi_jobs();
    if (!strcmp(n, "fg")) return bi_fg(argv);
    if (!strcmp(n, "bg")) return bi_bg(argv);
    if (!strcmp(n, "wait")) return bi_wait();
    return 127;
}

// ---- running a pipeline -----------------------------------------------------

typedef struct {
    char **argv;  // expanded words, NULL-terminated (argv[0] may be NULL: only redirections)
    Redir *redirs;
    int nredirs;
} Prepared;

static void prepare(const Command *c, Prepared *p) {
    p->argv = calloc((size_t)c->nwords + 1, sizeof *p->argv);
    for (int i = 0; i < c->nwords; ++i) p->argv[i] = expand(c->words[i]);
    p->nredirs = c->nredirs;
    p->redirs = calloc((size_t)c->nredirs + 1, sizeof *p->redirs);
    for (int i = 0; i < c->nredirs; ++i) {
        p->redirs[i].kind = c->redirs[i].kind;
        p->redirs[i].target = c->redirs[i].target ? expand(c->redirs[i].target) : NULL;
    }
}

static void prepared_free(Prepared *p) {
    for (int i = 0; p->argv[i]; ++i) free(p->argv[i]);
    free(p->argv);
    for (int i = 0; i < p->nredirs; ++i) free(p->redirs[i].target);
    free(p->redirs);
}

static int apply_redirs(const Prepared *p) {
    for (int i = 0; i < p->nredirs; ++i) {
        const Redir *r = &p->redirs[i];
        int fd, target;
        switch (r->kind) {
            case R_ERR_TO_OUT:
                if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) return -1;
                continue;
            case R_IN: fd = open(r->target, O_RDONLY); target = STDIN_FILENO; break;
            case R_OUT: fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0666); target = STDOUT_FILENO; break;
            case R_APPEND: fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0666); target = STDOUT_FILENO; break;
            default: fd = open(r->target, O_WRONLY | O_CREAT | O_TRUNC, 0666); target = STDERR_FILENO; break;
        }
        if (fd < 0) {
            fprintf(stderr, "msh: %s: %s\n", r->target, strerror(errno));
            return -1;
        }
        dup2(fd, target);
        close(fd);
    }
    return 0;
}

static int is_assignment(const char *w) {
    const char *eq = strchr(w, '=');
    return eq && valid_name(w, (size_t)(eq - w));
}

// Skip leading NAME=value words, setting them (for the child, or for the shell
// when they are the whole command). Returns the index of the first real word.
static int take_assignments(char **argv) {
    int i = 0;
    while (argv[i] && is_assignment(argv[i])) {
        char *eq = strchr(argv[i], '=');
        *eq = '\0';
        setenv(argv[i], eq + 1, 1);
        *eq = '=';
        i++;
    }
    return i;
}

// A single built-in (or bare assignment / redirection) runs inside the shell
// itself, so that `cd` and `export` take effect.
static int run_in_shell(Prepared *p) {
    int saved[3];
    for (int i = 0; i < 3; ++i) saved[i] = fcntl(i, F_DUPFD_CLOEXEC, 10);
    int status;
    fflush(stdout);
    if (apply_redirs(p) != 0) {
        status = 1;
    } else {
        int first = 0;
        while (p->argv[first] && is_assignment(p->argv[first])) first++;
        if (!p->argv[first]) {
            take_assignments(p->argv);
            status = 0;
        } else {
            status = run_builtin(p->argv + first);
        }
    }
    fflush(stdout);
    for (int i = 0; i < 3; ++i) {
        if (saved[i] >= 0) { dup2(saved[i], i); close(saved[i]); }
    }
    return status;
}

static void child_exec(Prepared *p, int bg) {
    if (!interactive && bg) {
        int fd = open("/dev/null", O_RDONLY);
        if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
    }
    if (apply_redirs(p) != 0) _exit(1);
    int first = take_assignments(p->argv);
    char **argv = p->argv + first;
    if (!argv[0]) _exit(0);
    if (is_builtin(argv[0])) {
        int s = run_builtin(argv);
        fflush(stdout);
        _exit(s);
    }
    execvp(argv[0], argv);
    int e = errno;
    fprintf(stderr, "msh: %s: %s\n", argv[0], e == ENOENT ? "command not found" : strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

static void pipeline_text(const Pipeline *pl, char *out, size_t n) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < pl->ncmds; ++i) {
        for (int w = 0; w < pl->cmds[i].nwords; ++w) {
            int k = snprintf(out + len, n - len, "%s%s", len ? " " : "", pl->cmds[i].words[w]);
            if (k < 0 || (size_t)k >= n - len) return;
            len += (size_t)k;
        }
        if (i + 1 < pl->ncmds) {
            int k = snprintf(out + len, n - len, " |");
            if (k < 0 || (size_t)k >= n - len) return;
            len += (size_t)k;
        }
    }
}

static void reset_signals(void) {
    signal(SIGINT, SIG_DFL);
    signal(SIGQUIT, SIG_DFL);
    signal(SIGTSTP, SIG_DFL);
    signal(SIGTTIN, SIG_DFL);
    signal(SIGTTOU, SIG_DFL);
}

static int run_pipeline(const Pipeline *pl, int bg) {
    int n = pl->ncmds;
    if (n > MAX_PIPE) {
        fprintf(stderr, "msh: too many commands in one pipeline (limit %d)\n", MAX_PIPE);
        return 1;
    }
    Prepared *prep = calloc((size_t)n, sizeof *prep);
    for (int i = 0; i < n; ++i) prepare(&pl->cmds[i], &prep[i]);

    int status;
    int single_in_shell = 0;
    if (n == 1 && !bg) {
        int first = 0;
        while (prep[0].argv[first] && is_assignment(prep[0].argv[first])) first++;
        // A bare assignment, a redirection-only command, or a built-in with no
        // assignment prefix runs inside the shell.
        single_in_shell = !prep[0].argv[first] ||
                          (first == 0 && is_builtin(prep[0].argv[0]));
    }

    if (single_in_shell) {
        status = run_in_shell(&prep[0]);
    } else {
        char text[200];
        pipeline_text(pl, text, sizeof text);
        Job *j = new_job(text);
        if (!j) {
            fprintf(stderr, "msh: too many jobs\n");
            status = 1;
            goto done;
        }
        fflush(stdout);
        fflush(stderr);
        int prev = -1;
        for (int i = 0; i < n; ++i) {
            int fds[2] = {-1, -1};
            if (i < n - 1 && pipe(fds) != 0) {
                perror("msh: pipe");
                break;
            }
            pid_t pid = fork();
            if (pid < 0) {
                perror("msh: fork");
                if (prev >= 0) close(prev);
                if (fds[0] >= 0) { close(fds[0]); close(fds[1]); }
                break;
            }
            if (pid == 0) {
                if (interactive) {
                    setpgid(0, i == 0 ? 0 : j->pgid);
                    if (!bg) tcsetpgrp(STDIN_FILENO, i == 0 ? getpid() : j->pgid);
                }
                reset_signals();
                if (prev >= 0) { dup2(prev, STDIN_FILENO); close(prev); }
                if (fds[1] >= 0) { dup2(fds[1], STDOUT_FILENO); close(fds[0]); close(fds[1]); }
                child_exec(&prep[i], bg);
            }
            if (i == 0) j->pgid = pid;
            if (interactive) setpgid(pid, j->pgid);
            j->pids[j->n++] = pid;
            if (prev >= 0) close(prev);
            if (fds[1] >= 0) close(fds[1]);
            prev = fds[0];
        }
        if (prev >= 0) close(prev);
        if (j->n == 0) {
            j->state = J_FREE;
            status = 1;
        } else if (bg) {
            j->bg = 1;
            if (interactive) fprintf(stderr, "[%d] %d\n", j->id, (int)j->pids[j->n - 1]);
            status = 0;
        } else {
            status = run_foreground(j);
        }
    }
done:
    for (int i = 0; i < n; ++i) prepared_free(&prep[i]);
    free(prep);
    return status;
}

// Run one line. Returns the status of the last command that ran.
static int run_line(const char *line) {
    List list;
    char err[200];
    if (parse_line(line, &list, err, sizeof err) != 0) {
        fprintf(stderr, "msh: %s\n", err);
        last_status = 2;
        return 2;
    }
    int status = last_status;
    for (int i = 0; i < list.nitems && !want_exit; ++i) {
        if (i > 0) {
            Link prev = list.items[i - 1].link;
            if ((prev == LINK_AND && status != 0) || (prev == LINK_OR && status == 0)) continue;
        }
        status = run_pipeline(&list.items[i].pipeline, list.items[i].link == LINK_BG);
        last_status = status;
    }
    list_free(&list);
    return status;
}

// ---- main -------------------------------------------------------------------

static void init_job_control(void) {
    setpgid(0, 0);  // fails harmlessly for a session leader
    shell_pgid = getpgrp();
    signal(SIGINT, SIG_IGN);
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGTTIN, SIG_IGN);
    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, shell_pgid);
}

static int run_stream(FILE *in) {
    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        if (interactive) {
            reap_jobs(1);
            fputs("msh$ ", stderr);
            fflush(stderr);
        } else {
            reap_jobs(0);
        }
        ssize_t n = getline(&line, &cap, in);
        if (n < 0) {
            if (interactive && errno == EINTR) { clearerr(in); continue; }
            break;
        }
        run_line(line);
        if (want_exit) break;
    }
    free(line);
    return want_exit ? exit_code : last_status;
}

int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        char *copy = strdup(argv[2]);
        for (char *line = strtok(copy, "\n"); line && !want_exit; line = strtok(NULL, "\n"))
            run_line(line);
        free(copy);
        return want_exit ? exit_code : last_status;
    }
    if (argc >= 2) {
        in = fopen(argv[1], "r");
        if (!in) {
            fprintf(stderr, "msh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
    } else if (isatty(STDIN_FILENO)) {
        interactive = 1;
        init_job_control();
    }
    return run_stream(in);
}
