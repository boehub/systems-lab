// Turning text into something a shell can run.
//
// Three small steps, kept apart so each can be tested without running any
// program: lex (text -> tokens), parse (tokens -> a tree of commands), and
// expand (a raw word -> the text the program receives: quotes removed,
// $VARIABLES, $(commands) and $((arithmetic)) filled in, split into words,
// wildcards matched).
#ifndef PARSE_H
#define PARSE_H

#include <stddef.h>

typedef enum { R_IN, R_OUT, R_APPEND, R_ERR, R_ERR_APPEND, R_ERR_TO_OUT, R_OUT_TO_ERR, R_HEREDOC } RedirKind;

typedef struct {
    RedirKind kind;
    char *target;  // raw word; for R_HEREDOC the text of the here-document; NULL for 2>&1
    int literal;   // R_HEREDOC: the delimiter was quoted, so the text is used as written
} Redir;

struct List;

typedef enum {
    C_SIMPLE,    // words, with redirections
    C_IF,        // if a; then b; else c; fi         (a, b, c)
    C_WHILE,     // while a; do b; done (or until)   (a, b)
    C_FOR,       // for var in words; do a; done     (var, words, a)
    C_CASE,      // case words[0] in pattern) ... esac   (arms)
    C_SUBSHELL,  // ( a )
    C_GROUP,     // { a; }
    C_FUNC       // name() { ...; }                  (words[0] is the name, src the body)
} CmdKind;

typedef struct {
    char **patterns;
    int npatterns;
    struct List *body;
} CaseArm;

typedef struct Command {
    CmdKind kind;
    char **words;  // raw words (quotes still in place); for a simple command words[0] is the program
    int nwords;
    Redir *redirs;
    int nredirs;
    char *var;           // C_FOR: the loop variable
    int has_in;          // C_FOR: an `in` list was given (otherwise the loop runs over "$@")
    int until;           // C_WHILE: written as `until`
    struct List *a, *b, *c;
    CaseArm *arms;
    int narms;
    char *src;           // C_FUNC: source text of the body, parsed again each time it is called
} Command;

typedef struct {
    Command *cmds;  // a | b | c
    int ncmds;
    int negate;     // started with `!`
} Pipeline;

// What follows an item: ';' or end of line, '&&', '||', or '&'.
typedef enum { LINK_SEQ, LINK_AND, LINK_OR, LINK_BG } Link;

typedef struct {
    Pipeline pipeline;
    Link link;
} Item;

typedef struct List {
    Item *items;
    int nitems;
} List;

// Parse a whole script (any number of lines). Returns
//    0  and fills `out` (possibly with no items for blank input or comments);
//   -1  with a message in `err` for a syntax error;
//    1  with a message in `err` when the text stops in the middle of something
//       (an open quote, `if` without `fi`, a trailing `|` or `&&`, a
//       here-document with no end marker). A terminal reads another line and
//       tries again; a script treats it as an error.
int parse_script(const char *text, List *out, char *err, size_t errsz);

// Like parse_script, but incomplete input is an error too (returns -1).
int parse_line(const char *line, List *out, char *err, size_t errsz);

void list_free(List *l);

// ---- expansion --------------------------------------------------------------

// Variable lookup. Returns NULL for "not set". Special names: "?" (last exit
// status), "$" (the shell's pid), "#" (number of positional parameters),
// "*" (all of them, joined by spaces), and "1", "2", ... for each one.
typedef const char *(*LookupFn)(const char *name, void *ctx);

// Runs `cmd` and returns what it printed (malloc'd, trailing newlines removed).
typedef char *(*CmdSubFn)(const char *cmd, void *ctx);

typedef struct {
    LookupFn lookup;
    CmdSubFn cmdsub;  // may be NULL: $(...) then stays as typed
    void *ctx;
} ExpandEnv;

// One word, no word splitting or wildcards: quotes and backslashes removed,
// $NAME ${NAME} ${NAME:-default} $? $$ $# $@ $* $1.. $(cmd) `cmd` $((1+2))
// filled in, and a leading ~. Caller frees.
char *expand_word_env(const char *raw, const ExpandEnv *env);
char *expand_word(const char *raw, LookupFn lookup, void *ctx);  // same, without $(...)

// Like expand_word, but for a word that may contain wildcards: quoted or
// escaped pattern characters (and anything that came from an expansion) come
// back backslash-escaped, and *has_glob says whether an unquoted * ? [ was
// seen. Pass the result to glob(3). Caller frees.
char *expand_pattern(const char *raw, LookupFn lookup, void *ctx, int *has_glob);

// A command argument: expand, split the results of unquoted $ expansions at
// white space ("$@" gives one word per parameter), then match wildcards (a
// pattern that matches nothing stays as typed). Returns a malloc'd array of
// malloc'd strings and sets *n; free each string and the array.
char **expand_fields(const char *raw, const ExpandEnv *env, int *n);

// The text of a here-document: only $ expansions, backticks and \$ \` \\ apply.
char *expand_heredoc(const char *text, const ExpandEnv *env);

// Evaluate an arithmetic expression such as "2 + x * 3". Names are looked up
// as variables. Returns 0 on success; -1 with a message in `err` otherwise.
int eval_arith(const char *expr, const ExpandEnv *env, long *result, char *err, size_t errsz);

#endif
