// Unit tests for the parser and word expansion (no programs are run).
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
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
    if (!strcmp(name, "STAR")) return "*";
    if (!strcmp(name, "EMPTY")) return "";
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

static int pattern_is(const char *raw, const char *want, int want_glob) {
    int g;
    char *got = expand_pattern(raw, lookup, NULL, &g);
    int ok = strcmp(got, want) == 0 && g == want_glob;
    if (!ok) printf("  pattern(%s) = [%s] glob=%d, wanted [%s] glob=%d\n", raw, got, g, want, want_glob);
    free(got);
    return ok;
}

static int incomplete(const char *s) {
    List l;
    char err[200] = "";
    int r = parse_script(s, &l, err, sizeof err);
    if (r == 0) list_free(&l);
    return r == 1 && err[0];
}

static char *fake_subst(const char *cmd, void *ctx) {
    (void)ctx;
    return strcmp(cmd, "words") == 0 ? strdup("a b  c") : strdup(cmd);
}

static int fields_are(const char *raw, int want_n, const char *want_joined) {
    ExpandEnv env = {lookup, fake_subst, NULL};
    int n;
    char **f = expand_fields(raw, &env, &n);
    char got[256] = "";
    for (int i = 0; i < n; ++i) {
        if (i) strcat(got, "|");
        strcat(got, f[i]);
        free(f[i]);
    }
    free(f);
    int ok = n == want_n && strcmp(got, want_joined) == 0;
    if (!ok) printf("  fields(%s) = %d [%s], wanted %d [%s]\n", raw, n, got, want_n, want_joined);
    return ok;
}

static int arith_is(const char *expr, long want) {
    ExpandEnv env = {lookup, NULL, NULL};
    long v = 0;
    char err[100];
    int rc = eval_arith(expr, &env, &v, err, sizeof err);
    if (rc != 0 || v != want) printf("  arith(%s) = %ld rc=%d, wanted %ld\n", expr, v, rc, want);
    return rc == 0 && v == want;
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

    CHECK(parse("a >&2 2>>log 1>&2", &l) == 0);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->nredirs == 3 && c->redirs[0].kind == R_OUT_TO_ERR && c->redirs[1].kind == R_ERR_APPEND);
        CHECK(c->redirs[2].kind == R_OUT_TO_ERR && c->nwords == 1);
    }
    list_free(&l);

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
    CHECK(expands_to("$5", ""));                      // positional parameter 5 is not set
    CHECK(expands_to("~", "/home/me"));
    CHECK(expands_to("~/docs", "/home/me/docs"));
    CHECK(expands_to("a~b", "a~b"));
    CHECK(expands_to("'~'", "~"));
    CHECK(expands_to("a'b c'd\"e f\"", "ab cde f"));  // quotes join into one word

    CHECK(pattern_is("*.c", "*.c", 1));
    CHECK(pattern_is("src/?.[ch]", "src/?.[ch]", 1));
    CHECK(pattern_is("plain", "plain", 0));
    CHECK(pattern_is("'*.c'", "\\*.c", 0));              // quoted: literal
    CHECK(pattern_is("\"a*\"", "a\\*", 0));
    CHECK(pattern_is("\\*", "\\*", 0));                 // backslash-escaped
    CHECK(pattern_is("\"x y\"*", "x y*", 1));            // quoted part plus a live *
    CHECK(pattern_is("$STAR", "\\*", 0));                // a variable's value is never a pattern
    CHECK(pattern_is("a\\b", "ab", 0));

    // compound commands
    CHECK(parse("if a; then b; elif c; then d; else e; fi", &l) == 0 && l.nitems == 1);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->kind == C_IF && c->a && c->b && c->c);
        CHECK(c->c->items[0].pipeline.cmds[0].kind == C_IF);  // the elif is a nested if
    }
    list_free(&l);
    CHECK(parse("while a; do b; done", &l) == 0 && l.items[0].pipeline.cmds[0].kind == C_WHILE); list_free(&l);
    CHECK(parse("until a; do b; done", &l) == 0 && l.items[0].pipeline.cmds[0].until == 1); list_free(&l);
    CHECK(parse("for x in 1 2 3; do echo $x; done", &l) == 0);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->kind == C_FOR && !strcmp(c->var, "x") && c->nwords == 3 && c->has_in);
    }
    list_free(&l);
    CHECK(parse("for x; do :; done", &l) == 0 && !l.items[0].pipeline.cmds[0].has_in); list_free(&l);
    CHECK(parse("case $x in a|b) one;; c) two;; *) three;; esac", &l) == 0);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->kind == C_CASE && c->narms == 3 && c->arms[0].npatterns == 2);
    }
    list_free(&l);
    CHECK(parse("( a; b ) | { c; d; } > out", &l) == 0 && l.items[0].pipeline.ncmds == 2);
    CHECK(l.items[0].pipeline.cmds[0].kind == C_SUBSHELL && l.items[0].pipeline.cmds[1].kind == C_GROUP);
    CHECK(l.items[0].pipeline.cmds[1].nredirs == 1);
    list_free(&l);
    CHECK(parse("f() { echo hi; }", &l) == 0);
    CHECK(l.items[0].pipeline.cmds[0].kind == C_FUNC && !strcmp(l.items[0].pipeline.cmds[0].words[0], "f"));
    CHECK(!strcmp(l.items[0].pipeline.cmds[0].src, "{ echo hi; }"));
    list_free(&l);
    CHECK(parse("! a | b", &l) == 0 && l.items[0].pipeline.negate && l.items[0].pipeline.ncmds == 2); list_free(&l);
    CHECK(parse("if a\nthen\n  b\nfi\n", &l) == 0 && l.nitems == 1); list_free(&l);
    CHECK(parse("a &&\n b", &l) == 0 && l.nitems == 2); list_free(&l);
    CHECK(parse("echo }", &l) == 0 && l.items[0].pipeline.cmds[0].nwords == 2); list_free(&l);  // } is only special at the start
    CHECK(parse("echo if then fi", &l) == 0 && l.items[0].pipeline.cmds[0].nwords == 4); list_free(&l);

    // here-documents
    CHECK(parse("cat <<EOF\nhello $x\nEOF\necho after", &l) == 0 && l.nitems == 2);
    {
        Command *c = &l.items[0].pipeline.cmds[0];
        CHECK(c->nredirs == 1 && c->redirs[0].kind == R_HEREDOC && !c->redirs[0].literal);
        CHECK(!strcmp(c->redirs[0].target, "hello $x\n"));
    }
    list_free(&l);
    CHECK(parse("cat <<'END'\n$raw\nEND\n", &l) == 0 && l.items[0].pipeline.cmds[0].redirs[0].literal); list_free(&l);
    CHECK(parse("cat <<-E\n\tx\n\tE\n", &l) == 0 && !strcmp(l.items[0].pipeline.cmds[0].redirs[0].target, "x\n")); list_free(&l);

    // incomplete input asks for more; real errors do not
    CHECK(incomplete("if a; then b"));
    CHECK(incomplete("for x in 1; do"));
    CHECK(incomplete("while a; do b"));
    CHECK(incomplete("case x in a)"));
    CHECK(incomplete("{ a;"));
    CHECK(incomplete("( a"));
    CHECK(incomplete("f() {"));
    CHECK(incomplete("a |"));
    CHECK(incomplete("a &&"));
    CHECK(incomplete("echo 'abc"));
    CHECK(incomplete("echo $(abc"));
    CHECK(incomplete("cat <<EOF\nabc\n"));
    CHECK(incomplete("echo a \\\n"));
    CHECK(!incomplete("if a; then b; fi"));
    CHECK(parse_fails("fi", "unexpected token fi"));
    CHECK(parse_fails("a )", "unexpected token )"));
    CHECK(parse_fails("if a; fi", "unexpected token fi"));
    CHECK(parse_fails("for 1x in a; do b; done", "unexpected token 1x"));
    CHECK(parse_fails("f() echo", "function body"));
    CHECK(parse_fails("a ( b )", "unexpected token ("));

    // splitting, substitution, arithmetic
    CHECK(fields_are("plain", 1, "plain"));
    CHECK(fields_are("$(words)", 3, "a|b|c"));          // unquoted: split
    CHECK(fields_are("\"$(words)\"", 1, "a b  c"));     // quoted: one word
    CHECK(fields_are("x$(words)y", 3, "xa|b|cy"));      // joins at the edges
    CHECK(fields_are("$EMPTY", 0, ""));                 // vanishes
    CHECK(fields_are("\"$EMPTY\"", 1, ""));              // an empty quoted word stays
    CHECK(fields_are("''", 1, ""));
    CHECK(fields_are("$SP", 2, "a|b"));
    CHECK(fields_are("\"$SP\"", 1, "a b"));
    CHECK(fields_are("`words`", 3, "a|b|c"));
    CHECK(fields_are("$((1+2))", 1, "3"));
    CHECK(fields_are("a\\ b", 1, "a b"));              // an escaped space stays in the word
    CHECK(arith_is("1 + 2 * 3", 7));
    CHECK(arith_is("(1 + 2) * 3", 9));
    CHECK(arith_is("10 / 3 + 10 % 3", 4));
    CHECK(arith_is("-4 + 10", 6));
    CHECK(arith_is("2 < 3", 1));
    CHECK(arith_is("2 >= 3", 0));
    CHECK(arith_is("1 && 0 || 1", 1));
    CHECK(arith_is("!0", 1));
    CHECK(arith_is("0x10 + 010", 24));
    {
        ExpandEnv env = {lookup, NULL, NULL};
        long v;
        char err[100];
        CHECK(eval_arith("1 / 0", &env, &v, err, sizeof err) != 0 && strstr(err, "division"));
        CHECK(eval_arith("1 +", &env, &v, err, sizeof err) != 0);
        CHECK(eval_arith("(1", &env, &v, err, sizeof err) != 0);
        CHECK(eval_arith("1 2", &env, &v, err, sizeof err) != 0);
    }
    {
        ExpandEnv env = {lookup, NULL, NULL};
        char *h = expand_heredoc("a $NAME \\$NAME \\\\ 'q' \"d\"", &env);
        CHECK(!strcmp(h, "a world $NAME \\ 'q' \"d\""));
        free(h);
    }

    // fuzz: random strings built from shell-ish pieces must never crash or leak
    {
        static const char *const bits[] = {"if", "then", "else", "elif", "fi", "while", "until", "do", "done",
            "for", "in", "case", "esac", "{", "}", "(", ")", ";", ";;", "|", "||", "&&", "&", "!", "<", ">",
            ">>", "2>", "2>&1", "<<", "<<-", "EOF", "\n", " ", "x", "f()", "$(", "$((", "`", "'", "\"", "\\",
            "$x", "${x:-y}", "*", "a=b", "function", "#", "\t", "echo"};
        const int nbits = (int)(sizeof bits / sizeof bits[0]);
        unsigned seed = 12345;
        fflush(stderr);
        int saved_err = dup(2), devnull = open("/dev/null", O_WRONLY);
        dup2(devnull, 2);  // the arithmetic errors the random text provokes are not interesting
        int parsed = 0;
        for (int iter = 0; iter < 60000; ++iter) {
            char text[400] = "";
            int pieces = 1 + (int)((seed = seed * 1103515245u + 12345u) >> 16) % 14;
            for (int k = 0; k < pieces; ++k) {
                seed = seed * 1103515245u + 12345u;
                strcat(text, bits[(seed >> 16) % (unsigned)nbits]);
                if ((seed >> 8) & 1) strcat(text, " ");
            }
            List fl;
            char err[200];
            if (parse_script(text, &fl, err, sizeof err) == 0) { list_free(&fl); parsed++; }
            ExpandEnv env = {lookup, fake_subst, NULL};
            int nf;
            char **f = expand_fields(text, &env, &nf);
            for (int i = 0; i < nf; ++i) free(f[i]);
            free(f);
        }
        fflush(stderr);
        dup2(saved_err, 2);
        close(saved_err);
        close(devnull);
        CHECK(parsed > 1000);  // plenty of the random inputs were valid, so the good paths were exercised too
    }

    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
