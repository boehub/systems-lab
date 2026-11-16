// Turning a line of text into something a shell can run.
//
// Three small steps, kept apart so each can be tested without running any
// program: lex (text -> tokens), parse (tokens -> a tree of commands), and
// expand (a raw word -> the text the program receives: quotes removed,
// $VARIABLES filled in).
#ifndef PARSE_H
#define PARSE_H

#include <stddef.h>

typedef enum { R_IN, R_OUT, R_APPEND, R_ERR, R_ERR_TO_OUT } RedirKind;

typedef struct {
    RedirKind kind;
    char *target;  // raw word; NULL for R_ERR_TO_OUT (2>&1)
} Redir;

typedef struct {
    char **words;  // raw words (quotes still in place); words[0] is the command
    int nwords;
    Redir *redirs;
    int nredirs;
} Command;

typedef struct {
    Command *cmds;  // a | b | c
    int ncmds;
} Pipeline;

// What follows an item: ';' or end of line, '&&', '||', or '&'.
typedef enum { LINK_SEQ, LINK_AND, LINK_OR, LINK_BG } Link;

typedef struct {
    Pipeline pipeline;
    Link link;
} Item;

typedef struct {
    Item *items;
    int nitems;
} List;

// Returns 0 and fills `out` (possibly with zero items for a blank line or a
// comment), or returns -1 and writes a message into `err`.
int parse_line(const char *line, List *out, char *err, size_t errsz);
void list_free(List *l);

// Variable lookup used by expand_word. Returns NULL for "not set".
// Special names: "?" (last exit status) and "$" (the shell's pid).
typedef const char *(*LookupFn)(const char *name, void *ctx);

// Remove quotes and backslashes, fill in $NAME, ${NAME}, $?, $$ and a leading
// ~. Single quotes keep everything literal. The result is one word (there is
// no word splitting or globbing). Caller frees.
char *expand_word(const char *raw, LookupFn lookup, void *ctx);

#endif
