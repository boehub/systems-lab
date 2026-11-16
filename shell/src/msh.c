// msh: a small Unix shell.
//
//   msh                 interactive (reads from the terminal)
//   msh script.sh ...   run a file (the rest of the words become $1, $2, ...)
//   msh -c 'cmd; cmd'   run a string
//
// Pipes, redirection, here-documents, && || ; &, quoting, $VARIABLES, $(...),
// $((...)), word splitting, wildcards, if / while / until / for / case,
// functions, ( ) and { }, a set of built-ins, and job control (Ctrl-C, Ctrl-Z,
// jobs, fg, bg). See README.md for what is left out.

#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
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

typedef struct { char *name, *src; } Func;
static Func funcs[128];
static int nfuncs;
static char **pos_args;     // $1 $2 ...
static int npos;
static const char *script_name = "msh";  // $0
static int break_n, continue_n;       // pending `break N` / `continue N`
static int returning, return_status, func_depth;
static int interrupted;     // a foreground job was killed by Ctrl-C: stop the running loops
static int cmdsub_status = -1;  // status of the last $(...) while preparing a command

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

static int run_text(const char *text);
static int exec_list(const List *l);
static void reset_signals(void);
static int decode(int st);

static const char *lookup_var(const char *name, void *ctx) {
    static char buf[32];
    static char *joined;
    (void)ctx;
    if (!strcmp(name, "?")) { snprintf(buf, sizeof buf, "%d", last_status); return buf; }
    if (!strcmp(name, "$")) { snprintf(buf, sizeof buf, "%d", (int)getpid()); return buf; }
    if (!strcmp(name, "#")) { snprintf(buf, sizeof buf, "%d", npos); return buf; }
    if (!strcmp(name, "0")) return script_name;
    if (name[0] >= '1' && name[0] <= '9' && !name[1]) {
        int k = name[0] - '0';
        return k <= npos ? pos_args[k - 1] : NULL;
    }
    if (name[0] >= '1' && name[0] <= '9') {  // $10 and up (reached through ${10} or "$@")
        int k = atoi(name);
        return k >= 1 && k <= npos ? pos_args[k - 1] : NULL;
    }
    if (!strcmp(name, "*")) {
        size_t len = 1;
        for (int i = 0; i < npos; ++i) len += strlen(pos_args[i]) + 1;
        joined = realloc(joined, len);
        joined[0] = '\0';
        for (int i = 0; i < npos; ++i) {
            if (i) strcat(joined, " ");
            strcat(joined, pos_args[i]);
        }
        return joined;
    }
    return getenv(name);  // every variable lives in the environment
}

// $(...): run the text in a child shell and collect what it prints.
static char *run_subst(const char *cmd, void *ctx) {
    (void)ctx;
    int fds[2];
    if (pipe(fds) != 0) return strdup("");
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return strdup(""); }
    if (pid == 0) {
        reset_signals();
        interactive = 0;
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        int st = run_text(cmd);
        fflush(stdout);
        _exit(want_exit ? exit_code : st);
    }
    close(fds[1]);
    size_t len = 0, cap = 256;
    char *out = malloc(cap);
    for (;;) {
        if (len + 128 > cap) { cap *= 2; out = realloc(out, cap); }
        ssize_t n = read(fds[0], out + len, cap - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fds[0]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    cmdsub_status = decode(st);
    while (len > 0 && out[len - 1] == '\n') len--;
    out[len] = '\0';
    return out;
}

static const ExpandEnv ENV = {lookup_var, run_subst, NULL};

static char *expand(const char *raw) { return expand_word_env(raw, &ENV); }

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
        if (WIFSIGNALED(st) && WTERMSIG(st) == SIGINT) interrupted = 1;
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
                                        "false", ":", "jobs", "fg", "bg", "wait", "test", "[",
                                        "read", "break", "continue", "return", "shift", "source",
                                        ".", "eval", "set", NULL};

static Func *find_func(const char *name) {
    for (int i = 0; i < nfuncs; ++i)
        if (!strcmp(funcs[i].name, name)) return &funcs[i];
    return NULL;
}

static int is_builtin(const char *name) {
    if (find_func(name)) return 1;
    for (int i = 0; kBuiltins[i]; ++i)
        if (!strcmp(name, kBuiltins[i])) return 1;
    return 0;
}

static void define_func(const char *name, const char *src) {
    Func *f = find_func(name);
    if (f) {
        free(f->src);
        f->src = strdup(src);
        return;
    }
    if (nfuncs == (int)(sizeof funcs / sizeof funcs[0])) {
        fprintf(stderr, "msh: too many functions\n");
        return;
    }
    funcs[nfuncs].name = strdup(name);
    funcs[nfuncs].src = strdup(src);
    nfuncs++;
}

static void set_positional(char **argv, int n) {
    char **copy = calloc((size_t)n + 1, sizeof *copy);
    for (int i = 0; i < n; ++i) copy[i] = strdup(argv[i]);
    for (int i = 0; i < npos; ++i) free(pos_args[i]);
    free(pos_args);
    pos_args = copy;
    npos = n;
}

static int call_func(const Func *f, char **argv) {
    char **saved = pos_args;
    int saved_n = npos;
    int n = 0;
    while (argv[1 + n]) n++;
    pos_args = NULL;
    npos = 0;
    set_positional(argv + 1, n);
    char *src = strdup(f->src);  // the function could be redefined while it runs
    func_depth++;
    int status = run_text(src);
    func_depth--;
    free(src);
    if (returning) { status = return_status; returning = 0; }
    for (int i = 0; i < npos; ++i) free(pos_args[i]);
    free(pos_args);
    pos_args = saved;
    npos = saved_n;
    return status;
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


// ---- test / [ ---------------------------------------------------------------

typedef struct {
    char **a;
    int n, i;
    int err;
} TestState;

static int t_expr(TestState *t);

static int t_is_binary(const char *op) {
    static const char *const ops[] = {"=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", NULL};
    for (int i = 0; ops[i]; ++i)
        if (!strcmp(op, ops[i])) return 1;
    return 0;
}

static int t_unary(const char *op, const char *arg) {
    struct stat st;
    if (!strcmp(op, "-n")) return *arg != '\0';
    if (!strcmp(op, "-z")) return *arg == '\0';
    if (!strcmp(op, "-e")) return stat(arg, &st) == 0;
    if (!strcmp(op, "-f")) return stat(arg, &st) == 0 && S_ISREG(st.st_mode);
    if (!strcmp(op, "-d")) return stat(arg, &st) == 0 && S_ISDIR(st.st_mode);
    if (!strcmp(op, "-s")) return stat(arg, &st) == 0 && st.st_size > 0;
    if (!strcmp(op, "-L") || !strcmp(op, "-h")) return lstat(arg, &st) == 0 && S_ISLNK(st.st_mode);
    if (!strcmp(op, "-r")) return access(arg, R_OK) == 0;
    if (!strcmp(op, "-w")) return access(arg, W_OK) == 0;
    if (!strcmp(op, "-x")) return access(arg, X_OK) == 0;
    return -1;
}

static int t_primary(TestState *t) {
    if (t->i >= t->n) { t->err = 1; return 0; }
    const char *a = t->a[t->i];
    if (!strcmp(a, "(") && t->i + 1 < t->n) {
        t->i++;
        int v = t_expr(t);
        if (t->i < t->n && !strcmp(t->a[t->i], ")")) t->i++;
        else t->err = 1;
        return v;
    }
    if (t->i + 2 < t->n + 0 && t_is_binary(t->a[t->i + 1])) {
        const char *l = t->a[t->i], *op = t->a[t->i + 1], *r = t->a[t->i + 2];
        t->i += 3;
        if (!strcmp(op, "=") || !strcmp(op, "==")) return !strcmp(l, r);
        if (!strcmp(op, "!=")) return strcmp(l, r) != 0;
        char *e1, *e2;
        long x = strtol(l, &e1, 10), y = strtol(r, &e2, 10);
        if (!*l || *e1 || !*r || *e2) { t->err = 1; return 0; }
        if (!strcmp(op, "-eq")) return x == y;
        if (!strcmp(op, "-ne")) return x != y;
        if (!strcmp(op, "-lt")) return x < y;
        if (!strcmp(op, "-le")) return x <= y;
        if (!strcmp(op, "-gt")) return x > y;
        return x >= y;
    }
    if (t->i + 1 < t->n && a[0] == '-' && a[1] && !a[2]) {
        int v = t_unary(a, t->a[t->i + 1]);
        if (v >= 0) { t->i += 2; return v; }
    }
    t->i++;
    return *a != '\0';
}

static int t_not(TestState *t) {
    if (t->i + 1 < t->n && !strcmp(t->a[t->i], "!")) {
        t->i++;
        return !t_not(t);
    }
    return t_primary(t);
}

static int t_and(TestState *t) {
    int v = t_not(t);
    while (t->i < t->n && !strcmp(t->a[t->i], "-a")) {
        t->i++;
        int r = t_not(t);
        v = v && r;
    }
    return v;
}

static int t_expr(TestState *t) {
    int v = t_and(t);
    while (t->i < t->n && !strcmp(t->a[t->i], "-o")) {
        t->i++;
        int r = t_and(t);
        v = v || r;
    }
    return v;
}

// Returns 0 for true, 1 for false, 2 for a usage error.
static int bi_test(char **argv, int bracket) {
    int n = 0;
    while (argv[1 + n]) n++;
    if (bracket) {
        if (n == 0 || strcmp(argv[n], "]")) {
            fprintf(stderr, "msh: [: missing ]\n");
            return 2;
        }
        n--;
    }
    if (n == 0) return 1;
    TestState t = {argv + 1, n, 0, 0};
    int v = t_expr(&t);
    if (t.err || t.i != t.n) {
        fprintf(stderr, "msh: %s: bad expression\n", bracket ? "[" : "test");
        return 2;
    }
    return v ? 0 : 1;
}

// ---- read -------------------------------------------------------------------

static int bi_read(char **argv) {
    int raw = 0, i = 1;
    if (argv[i] && !strcmp(argv[i], "-r")) { raw = 1; i++; }
    char **names = argv + i;
    static char *line;
    size_t len = 0, cap = 0;
    int got_any = 0, eof = 0;
    free(line);
    line = NULL;
    for (;;) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { eof = 1; break; }
        got_any = 1;
        if (c == '\n') break;
        if (c == '\\' && !raw) {
            char d;
            if (read(STDIN_FILENO, &d, 1) != 1) break;
            if (d == '\n') continue;  // line continuation
            c = d;
        }
        if (len + 2 > cap) { cap = cap ? cap * 2 : 64; line = realloc(line, cap); }
        line[len++] = c;
    }
    if (!line) line = strdup("");
    line[len] = '\0';
    char *p = line;
    if (!names[0]) {
        setenv("REPLY", line, 1);
    } else {
        for (int k = 0; names[k]; ++k) {
            while (*p == ' ' || *p == '\t') p++;
            if (!names[k + 1]) {  // the last name takes the rest of the line
                char *end = p + strlen(p);
                while (end > p && (end[-1] == ' ' || end[-1] == '\t')) end--;
                char save = *end;
                *end = '\0';
                setenv(names[k], p, 1);
                *end = save;
            } else {
                char *w = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                char save = *p;
                *p = '\0';
                setenv(names[k], w, 1);
                *p = save;
            }
        }
    }
    return (eof && !got_any) ? 1 : 0;
}

static int bi_shift(char **argv) {
    int n = argv[1] ? atoi(argv[1]) : 1;
    if (n < 0 || n > npos) { fprintf(stderr, "msh: shift: shift count out of range\n"); return 1; }
    for (int i = 0; i < n; ++i) free(pos_args[i]);
    memmove(pos_args, pos_args + n, (size_t)(npos - n) * sizeof *pos_args);
    npos -= n;
    return 0;
}

static int bi_set(char **argv) {
    if (argv[1] && !strcmp(argv[1], "--")) {
        int n = 0;
        while (argv[2 + n]) n++;
        set_positional(argv + 2, n);
        return 0;
    }
    if (argv[1]) {
        fprintf(stderr, "msh: set: only `set --` is supported\n");
        return 1;
    }
    extern char **environ;
    for (char **e = environ; *e; ++e) printf("%s\n", *e);
    fflush(stdout);
    return 0;
}

static int bi_source(char **argv) {
    if (!argv[1]) { fprintf(stderr, "msh: %s: filename argument required\n", argv[0]); return 2; }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "msh: %s: %s\n", argv[1], strerror(errno)); return 1; }
    size_t len = 0, cap = 4096;
    char *text = malloc(cap);
    for (;;) {
        size_t n = fread(text + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0) break;
        if (len + 1 >= cap) { cap *= 2; text = realloc(text, cap); }
    }
    fclose(f);
    text[len] = '\0';
    char **saved = pos_args;
    int saved_n = npos;
    int n = 0;
    while (argv[2 + n]) n++;
    if (n) {
        pos_args = NULL;
        npos = 0;
        set_positional(argv + 2, n);
    }
    int status = run_text(text);
    if (n) {
        for (int i = 0; i < npos; ++i) free(pos_args[i]);
        free(pos_args);
        pos_args = saved;
        npos = saved_n;
    }
    free(text);
    if (returning && func_depth == 0) { returning = 0; status = return_status; }
    return status;
}

static int bi_eval(char **argv) {
    size_t len = 1;
    for (int i = 1; argv[i]; ++i) len += strlen(argv[i]) + 1;
    char *text = malloc(len);
    text[0] = '\0';
    for (int i = 1; argv[i]; ++i) {
        if (i > 1) strcat(text, " ");
        strcat(text, argv[i]);
    }
    int status = run_text(text);
    free(text);
    return status;
}

static int run_builtin(char **argv) {
    const char *n = argv[0];
    const Func *fn = find_func(n);
    if (fn) return call_func(fn, argv);
    if (!strcmp(n, "test")) return bi_test(argv, 0);
    if (!strcmp(n, "[")) return bi_test(argv, 1);
    if (!strcmp(n, "read")) return bi_read(argv);
    if (!strcmp(n, "shift")) return bi_shift(argv);
    if (!strcmp(n, "set")) return bi_set(argv);
    if (!strcmp(n, "source") || !strcmp(n, ".")) return bi_source(argv);
    if (!strcmp(n, "eval")) return bi_eval(argv);
    if (!strcmp(n, "break") || !strcmp(n, "continue")) {
        int levels = argv[1] ? atoi(argv[1]) : 1;
        if (levels < 1) levels = 1;
        if (n[0] == 'b') break_n = levels; else continue_n = levels;
        return 0;
    }
    if (!strcmp(n, "return")) {
        returning = 1;
        return_status = argv[1] ? atoi(argv[1]) & 0xFF : last_status;
        return return_status;
    }
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
    const Command *cmd;
    char **argv;  // expanded words, NULL-terminated (argv[0] may be NULL: only redirections)
    Redir *redirs;
    int nredirs;
} Prepared;

static int is_assignment(const char *w) {
    const char *eq = strchr(w, '=');
    return eq && valid_name(w, (size_t)(eq - w));
}

static void prepare(const Command *c, Prepared *p) {
    p->cmd = c;
    int n = 0, cap = c->kind == C_SIMPLE ? c->nwords + 1 : 1;
    p->argv = calloc((size_t)cap, sizeof *p->argv);
    int prefix = 1;  // still inside the leading NAME=value words
    for (int i = 0; c->kind == C_SIMPLE && i < c->nwords; ++i) {
        char **f;
        int nf;
        if (prefix && is_assignment(c->words[i])) {
            f = malloc(sizeof *f);  // an assignment is one word: no splitting, no wildcards
            f[0] = expand(c->words[i]);
            nf = 1;
        } else {
            prefix = 0;
            f = expand_fields(c->words[i], &ENV, &nf);
        }
        if (n + nf + 1 > cap) {
            cap = (n + nf + 1) * 2;
            p->argv = realloc(p->argv, (size_t)cap * sizeof *p->argv);
        }
        for (int k = 0; k < nf; ++k) p->argv[n++] = f[k];
        free(f);
    }
    p->argv[n] = NULL;
    p->nredirs = c->nredirs;
    p->redirs = calloc((size_t)c->nredirs + 1, sizeof *p->redirs);
    for (int i = 0; i < c->nredirs; ++i) {
        p->redirs[i].kind = c->redirs[i].kind;
        p->redirs[i].literal = c->redirs[i].literal;
        if (c->redirs[i].kind == R_HEREDOC) p->redirs[i].target = strdup(c->redirs[i].target);  // expanded when used
        else p->redirs[i].target = c->redirs[i].target ? expand(c->redirs[i].target) : NULL;
    }
}

static void prepared_free(Prepared *p) {
    for (int i = 0; p->argv[i]; ++i) free(p->argv[i]);
    free(p->argv);
    for (int i = 0; i < p->nredirs; ++i) free(p->redirs[i].target);
    free(p->redirs);
}

// Make `text` readable on fd 0 (a here-document).
static int heredoc_stdin(const char *text) {
    FILE *tf = tmpfile();
    if (!tf) return -1;
    size_t len = strlen(text);
    if (fwrite(text, 1, len, tf) != len || fflush(tf) != 0) { fclose(tf); return -1; }
    int fd = dup(fileno(tf));
    fclose(tf);
    if (fd < 0) return -1;
    lseek(fd, 0, SEEK_SET);
    dup2(fd, STDIN_FILENO);
    close(fd);
    return 0;
}

static int apply_redirs(const Prepared *p) {
    for (int i = 0; i < p->nredirs; ++i) {
        const Redir *r = &p->redirs[i];
        int fd, target;
        switch (r->kind) {
            case R_ERR_TO_OUT:
                if (dup2(STDOUT_FILENO, STDERR_FILENO) < 0) return -1;
                continue;
            case R_OUT_TO_ERR:
                if (dup2(STDERR_FILENO, STDOUT_FILENO) < 0) return -1;
                continue;
            case R_ERR_APPEND: fd = open(r->target, O_WRONLY | O_CREAT | O_APPEND, 0666); target = STDERR_FILENO; break;
            case R_HEREDOC: {
                char *text = r->literal ? strdup(r->target) : expand_heredoc(r->target, &ENV);
                int rc = heredoc_stdin(text);
                free(text);
                if (rc != 0) { fprintf(stderr, "msh: cannot create a temporary file for a here-document\n"); return -1; }
                continue;
            }
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

// ---- compound commands ----

// After a loop body: should this loop stop? Handles `break N` and `continue N`.
static int loop_stops(void) {
    if (want_exit || returning || interrupted) return 1;
    if (break_n) { break_n--; return 1; }
    if (continue_n) {
        continue_n--;
        return continue_n > 0;  // `continue 2`: leave this loop, the outer one continues
    }
    return 0;
}

static int exec_for(const Command *c) {
    char **items = NULL;
    int n = 0, cap = 0;
    if (c->has_in) {
        for (int i = 0; i < c->nwords; ++i) {
            int nf;
            char **f = expand_fields(c->words[i], &ENV, &nf);
            if (n + nf > cap) { cap = (n + nf) * 2; items = realloc(items, (size_t)cap * sizeof *items); }
            for (int k = 0; k < nf; ++k) items[n++] = f[k];
            free(f);
        }
    } else {
        items = malloc((size_t)(npos + 1) * sizeof *items);
        for (int i = 0; i < npos; ++i) items[n++] = strdup(pos_args[i]);
    }
    int status = 0;
    for (int i = 0; i < n; ++i) {
        setenv(c->var, items[i], 1);
        status = exec_list(c->a);
        if (loop_stops()) break;
    }
    for (int i = 0; i < n; ++i) free(items[i]);
    free(items);
    return status;
}

static int exec_case(const Command *c) {
    char *subject = expand(c->words[0]);
    int status = 0;
    for (int i = 0; i < c->narms; ++i) {
        int hit = 0;
        for (int k = 0; k < c->arms[i].npatterns && !hit; ++k) {
            int has_glob;
            char *pat = expand_pattern(c->arms[i].patterns[k], lookup_var, NULL, &has_glob);
            hit = fnmatch(pat, subject, 0) == 0;
            free(pat);
        }
        if (hit) {
            status = exec_list(c->arms[i].body);
            break;
        }
    }
    free(subject);
    return status;
}

static int exec_compound(const Command *c) {
    int status = 0;
    switch (c->kind) {
        case C_IF:
            status = exec_list(c->a);
            if (want_exit || returning || break_n || continue_n || interrupted) return status;
            if (status == 0) return exec_list(c->b);
            return c->c ? exec_list(c->c) : 0;
        case C_WHILE:
            for (;;) {
                int cond = exec_list(c->a);
                if (want_exit || returning || break_n || continue_n || interrupted) { loop_stops(); break; }
                if ((cond == 0) == (c->until != 0)) break;
                status = exec_list(c->b);
                if (loop_stops()) break;
            }
            return status;
        case C_FOR: return exec_for(c);
        case C_CASE: return exec_case(c);
        case C_GROUP:
        case C_SUBSHELL: return exec_list(c->a);
        case C_FUNC: define_func(c->words[0], c->src); return 0;
        default: return 0;
    }
}

// A single built-in (or bare assignment / redirection / compound command) runs
// inside the shell itself, so that `cd`, `export` and `x=1` take effect.
static int run_in_shell(Prepared *p) {
    int saved[3];
    for (int i = 0; i < 3; ++i) saved[i] = fcntl(i, F_DUPFD_CLOEXEC, 10);
    int status;
    fflush(stdout);
    if (apply_redirs(p) != 0) {
        status = 1;
    } else if (p->cmd->kind != C_SIMPLE) {
        status = exec_compound(p->cmd);
    } else {
        int first = 0;
        while (p->argv[first] && is_assignment(p->argv[first])) first++;
        if (!p->argv[first]) {
            take_assignments(p->argv);
            status = cmdsub_status >= 0 ? cmdsub_status : 0;  // x=$(false) fails
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
    if (p->cmd->kind != C_SIMPLE) {  // a compound command in a pipeline, a subshell, or in the background
        interactive = 0;
        int s = exec_compound(p->cmd);
        fflush(stdout);
        fflush(stderr);
        _exit(want_exit ? exit_code : s);
    }
    int first = take_assignments(p->argv);
    char **argv = p->argv + first;
    if (!argv[0]) _exit(0);
    if (is_builtin(argv[0])) {
        interactive = 0;
        int s = run_builtin(argv);
        fflush(stdout);
        _exit(want_exit ? exit_code : s);
    }
    execvp(argv[0], argv);
    int e = errno;
    fprintf(stderr, "msh: %s: %s\n", argv[0], e == ENOENT ? "command not found" : strerror(e));
    _exit(e == ENOENT ? 127 : 126);
}

static const char *compound_label(CmdKind k) {
    switch (k) {
        case C_IF: return "if ...";
        case C_WHILE: return "while ...";
        case C_FOR: return "for ...";
        case C_CASE: return "case ...";
        case C_SUBSHELL: return "( ... )";
        case C_GROUP: return "{ ...; }";
        default: return "...";
    }
}

static void pipeline_text(const Pipeline *pl, char *out, size_t n) {
    size_t len = 0;
    out[0] = '\0';
    for (int i = 0; i < pl->ncmds; ++i) {
        if (pl->cmds[i].kind != C_SIMPLE) {
            int k = snprintf(out + len, n - len, "%s%s", len ? " " : "", compound_label(pl->cmds[i].kind));
            if (k < 0 || (size_t)k >= n - len) return;
            len += (size_t)k;
        }
        for (int w = 0; pl->cmds[i].kind == C_SIMPLE && w < pl->cmds[i].nwords; ++w) {
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
    cmdsub_status = -1;
    for (int i = 0; i < n; ++i) prepare(&pl->cmds[i], &prep[i]);

    int status;
    int single_in_shell = 0;
    if (n == 1 && !bg) {
        if (pl->cmds[0].kind == C_SIMPLE) {
            int first = 0;
            while (prep[0].argv[first] && is_assignment(prep[0].argv[first])) first++;
            // A bare assignment, a redirection-only command, or a built-in with no
            // assignment prefix runs inside the shell.
            single_in_shell = !prep[0].argv[first] ||
                              (first == 0 && is_builtin(prep[0].argv[0]));
        } else {
            single_in_shell = pl->cmds[0].kind != C_SUBSHELL;
        }
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
    if (pl->negate) status = status == 0 ? 1 : 0;
    return status;
}

// Run a list of pipelines joined by ; && || &. Returns the status of the last one that ran.
static int exec_list(const List *l) {
    int status = last_status;
    for (int i = 0; i < l->nitems; ++i) {
        if (want_exit || returning || break_n || continue_n || interrupted) break;
        if (i > 0) {
            Link prev = l->items[i - 1].link;
            if ((prev == LINK_AND && status != 0) || (prev == LINK_OR && status == 0)) continue;
        }
        status = run_pipeline(&l->items[i].pipeline, l->items[i].link == LINK_BG);
        last_status = status;
    }
    return status;
}

// Parse and run a whole piece of script text.
static int run_text(const char *text) {
    List list;
    char err[200];
    if (parse_script(text, &list, err, sizeof err) != 0) {
        fprintf(stderr, "msh: %s\n", err);
        last_status = 2;
        return 2;
    }
    int status = exec_list(&list);
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

// Read commands from a file or terminal. Lines are collected until they form
// a complete command (an `if` waits for its `fi`, a quote for its closing
// quote, a `<<EOF` for its end marker), then run.
static int run_stream(FILE *in) {
    char *line = NULL;
    size_t cap = 0;
    char *acc = NULL;
    size_t acc_len = 0;
    for (;;) {
        if (interactive) {
            reap_jobs(1);
            fputs(acc_len ? "> " : "msh$ ", stderr);
            fflush(stderr);
        } else {
            reap_jobs(0);
        }
        ssize_t n = getline(&line, &cap, in);
        if (n < 0) {
            if (interactive && errno == EINTR) { clearerr(in); continue; }
            break;
        }
        acc = realloc(acc, acc_len + (size_t)n + 2);
        memcpy(acc + acc_len, line, (size_t)n);
        acc_len += (size_t)n;
        if (acc[acc_len - 1] != '\n') acc[acc_len++] = '\n';
        acc[acc_len] = '\0';

        List list;
        char err[200];
        int rc = parse_script(acc, &list, err, sizeof err);
        if (rc == 1) continue;  // not finished: read another line
        if (rc == 0) {
            interrupted = 0;
            exec_list(&list);
            list_free(&list);
        } else {
            fprintf(stderr, "msh: %s\n", err);
            last_status = 2;
        }
        acc_len = 0;
        if (want_exit) break;
    }
    if (acc_len && !want_exit) {  // the input ended in the middle of a command
        List list;
        char err[200];
        parse_script(acc, &list, err, sizeof err);
        fprintf(stderr, "msh: %s\n", err);
        last_status = 2;
    }
    free(acc);
    free(line);
    return want_exit ? exit_code : last_status;
}

int main(int argc, char **argv) {
    FILE *in = stdin;
    if (argc >= 3 && !strcmp(argv[1], "-c")) {
        if (argc > 3) {
            script_name = argv[3];
            set_positional(argv + 4, argc - 4);
        }
        run_text(argv[2]);
        return want_exit ? exit_code : last_status;
    }
    if (argc >= 2) {
        in = fopen(argv[1], "r");
        if (!in) {
            fprintf(stderr, "msh: %s: %s\n", argv[1], strerror(errno));
            return 127;
        }
        script_name = argv[1];
        set_positional(argv + 2, argc - 2);
    } else if (isatty(STDIN_FILENO)) {
        interactive = 1;
        init_job_control();
    }
    return run_stream(in);
}
