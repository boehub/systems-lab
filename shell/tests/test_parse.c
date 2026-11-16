// Unit tests for the parser and word expansion (no programs are run).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "parse.h"

static int checks, failed;

#define CHECK(cond)                                                     \
    do {                                                                \
        checks++;                                                       \
        if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)

static const char *lookup(const char *name, void *ctx) {
    (void)ctx;
    if (!strcmp(name, "NAME")) return "world";
    if (!strcmp(name, "HOME")) return "/home/me";
    if (!strcmp(name, "?")) return "3";
    if (!strcmp(name, "SP")) return "a b";
    return NULL;
}

static int parse(const char *s, List *l) {
    char err[200];
    return parse_line(s, l, err, sizeof err);
}

static int parse_fails(const char *s, const char *needle) {
    List l;
    char err[200] = "";
    int r = parse_line(s, &l, err, sizeof err);
    if (r == 0) { list_free(&l); return 0; }
    return strstr(err, needle) != NULL;
}

static int expands_to(const char *raw, const char *want) {
    char *got = expand_word(raw, lookup, NULL);
    int ok = strcmp(got, want) == 0;
    if (!ok) printf("  expand(%s) = [%s], wanted [%s]\n", raw, got, want);
    free(got);
    return ok;
}

int main(void) {
    List l;

    // blank lines and comments
    CHECK(parse("", &l) == 0 && l.nitems == 0); list_free(&l);
    CHECK(parse("   \t ", &l) == 0 && l.nitems == 0); list_free(&l);
    CHECK(parse("# nothing here", &l) == 0 && l.nitems == 0); list_free(&l);

    // one simple command
    CHECK(parse("ls -l /tmp", &l) == 0);
    CHECK(l.nitems == 1 && l.items[0].pipeline.ncmds == 1);
    CHECK(l.items[0].pipeline.cmds[0].nwords == 3);
    CHECK(!strcmp(l.items[0].pipeline.cmds[0].words[2], "/tmp"));
    list_free(&l);

    // a comment after a command is dropped; # inside a word is not a comment
    CHECK(parse("echo hi # not this", &l) == 0 && l.items[0].pipeline.cmds[0].nwords == 2); list_free(&l);
    CHECK(parse("echo a#b", &l) == 0 && !strcmp(l.items[0].pipeline.cmds[0].words[1], "a#b")); list_free(&l);

    // pipelines
    CHECK(parse("a | b | c", &l) == 0);
    CHECK(l.nitems == 1 && l.items[0].pipeline.ncmds == 3);
    CHECK(!strcmp(l.items[0].pipeline.cmds[2].words[0], "c"));
    list_free(&l);

    // list operators and what follows each item
    CHECK(parse("a; b && c || d &", &l) == 0 && l.nitems == 4);
    CHECK(l.items[0].link == LINK_SEQ && l.items[1].link == LINK_AND);
    CHECK(l.items[2].link == LINK_OR && l.items[3].link == LINK_BG);
    list_free(&l);
    CHECK(parse("a;", &l) == 0 && l.nitems == 1); list_free(&l);

    // redirections
    CHECK(parse("sort < in.txt > out.txt 2>err.txt", &l) == 0);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->nwords == 1 && c->nredirs == 3);
        CHECK(c->redirs[0].kind == R_IN && !strcmp(c->redirs[0].target, "in.txt"));
        CHECK(c->redirs[1].kind == R_OUT && !strcmp(c->redirs[1].target, "out.txt"));
        CHECK(c->redirs[2].kind == R_ERR && !strcmp(c->redirs[2].target, "err.txt"));
    }
    list_free(&l);
    CHECK(parse("cmd >> log 2>&1", &l) == 0);
    CHECK(l.items[0].pipeline.cmds[0].redirs[0].kind == R_APPEND);
    CHECK(l.items[0].pipeline.cmds[0].redirs[1].kind == R_ERR_TO_OUT);
    list_free(&l);
    CHECK(parse("> only-a-file", &l) == 0 && l.items[0].pipeline.cmds[0].nwords == 0); list_free(&l);

    // quotes protect operators and spaces
    CHECK(parse("echo 'a | b' \"c && d\" e\\;f", &l) == 0);
    CHECK(l.items[0].pipeline.ncmds == 1 && l.items[0].pipeline.cmds[0].nwords == 4);
    list_free(&l);

    // errors
    CHECK(parse_fails("echo 'oops", "unterminated single"));
    CHECK(parse_fails("echo \"oops", "unterminated double"));
    CHECK(parse_fails("| a", "near unexpected token |"));
    CHECK(parse_fails("a |", "near unexpected token newline"));
    CHECK(parse_fails("a | | b", "near unexpected token |"));
    CHECK(parse_fails("a && ", "nothing after &&"));
    CHECK(parse_fails("a ;; b", "near unexpected token ;"));
    CHECK(parse_fails("a >", "missing file name"));
    CHECK(parse_fails("a > > b", "missing file name"));

    // expansion
    CHECK(expands_to("plain", "plain"));
    CHECK(expands_to("$NAME", "world"));
    CHECK(expands_to("${NAME}ly", "worldly"));
    CHECK(expands_to("$NAMEly", ""));                 // that is a different variable
    CHECK(expands_to("hello-$NAME!", "hello-world!"));
    CHECK(expands_to("'$NAME'", "$NAME"));            // single quotes are literal
    CHECK(expands_to("\"$NAME\"", "world"));
    CHECK(expands_to("\"a  b\"", "a  b"));
    CHECK(expands_to("\"$SP\"", "a b"));
    CHECK(expands_to("\\$NAME", "$NAME"));
    CHECK(expands_to("\"\\$NAME\"", "$NAME"));
    CHECK(expands_to("\"say \\\"hi\\\"\"", "say \"hi\""));
    CHECK(expands_to("$?", "3"));
    CHECK(expands_to("$UNSET", ""));
    CHECK(expands_to("cost$", "cost$"));              // a lone $ stays
    CHECK(expands_to("$5", "$5"));                    // positional parameters aren't supported
    CHECK(expands_to("~", "/home/me"));
    CHECK(expands_to("~/docs", "/home/me/docs"));
    CHECK(expands_to("a~b", "a~b"));
    CHECK(expands_to("'~'", "~"));
    CHECK(expands_to("a'b c'd\"e f\"", "ab cde f"));  // quotes join into one word

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
