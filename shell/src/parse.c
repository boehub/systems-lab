#include "parse.h"

#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INCOMPLETE (-2)  // internal code: the text ended too soon

typedef enum {
    T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_DSEMI, T_AMP,
    T_LT, T_GT, T_APPEND, T_ERRGT, T_ERRAPP, T_ERR2OUT, T_OUT2ERR, T_HEREDOC,
    T_LPAREN, T_RPAREN, T_NL, T_END
} TokKind;

typedef struct {
    TokKind kind;
    char *text;       // T_WORD: the raw word; T_HEREDOC: the end marker
    size_t start, end;
    char *body;       // T_HEREDOC: the text up to the end marker
    int strip_tabs;   // T_HEREDOC: written <<-
    int literal;      // T_HEREDOC: marker was quoted
} Token;

static int set_msg(char *err, size_t n, const char *a, const char *b) {
    if (err && n) snprintf(err, n, "%s%s", a, b);
    return -1;
}

static const char *tok_name(TokKind k) {
    switch (k) {
        case T_PIPE: return "|";
        case T_AND: return "&&";
        case T_OR: return "||";
        case T_SEMI: return ";";
        case T_DSEMI: return ";;";
        case T_AMP: return "&";
        case T_LT: return "<";
        case T_GT: return ">";
        case T_APPEND: return ">>";
        case T_ERRGT: return "2>";
        case T_ERRAPP: return "2>>";
        case T_ERR2OUT: return "2>&1";
        case T_OUT2ERR: return ">&2";
        case T_HEREDOC: return "<<";
        case T_LPAREN: return "(";
        case T_RPAREN: return ")";
        default: return "newline";
    }
}

static void free_tokens(Token *t, int n) {
    for (int i = 0; i < n; ++i) {
        free(t[i].text);
        free(t[i].body);
    }
    free(t);
}

// ---- growable byte buffer ---------------------------------------------------

typedef struct {
    char *p;
    size_t n, cap;
} Buf;

static void put(Buf *b, const char *s, size_t len) {
    if (b->n + len + 1 > b->cap) {
        b->cap = (b->n + len + 1) * 2;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, s, len);
    b->n += len;
    b->p[b->n] = '\0';
}

// ---- lexer ------------------------------------------------------------------

static int is_blank(char c) { return c == ' ' || c == '\t' || c == '\r'; }

// s[i..] starts with "$(". Returns the index just past the matching ')', or 0
// when it is never closed. Understands quotes and nested substitutions.
static size_t skip_subst(const char *s, size_t i) {
    size_t j = i + 2;
    int depth = 1;
    while (s[j]) {
        char c = s[j];
        if (c == '\\' && s[j + 1]) {
            j += 2;
        } else if (c == '\'') {
            const char *e = strchr(s + j + 1, '\'');
            if (!e) return 0;
            j = (size_t)(e - s) + 1;
        } else if (c == '"') {
            j++;
            while (s[j] && s[j] != '"') {
                if (s[j] == '\\' && s[j + 1]) j += 2;
                else if (s[j] == '$' && s[j + 1] == '(') {
                    size_t e = skip_subst(s, j);
                    if (!e) return 0;
                    j = e;
                } else j++;
            }
            if (!s[j]) return 0;
            j++;
        } else if (c == '(') {
            depth++;
            j++;
        } else if (c == ')') {
            if (--depth == 0) return j + 1;
            j++;
        } else {
            j++;
        }
    }
    return 0;
}

// s[i] is a backquote. Returns the index past the closing one, or 0.
static size_t skip_backtick(const char *s, size_t i) {
    size_t j = i + 1;
    while (s[j]) {
        if (s[j] == '\\' && s[j + 1]) j += 2;
        else if (s[j] == '`') return j + 1;
        else j++;
    }
    return 0;
}

// Scan one word starting at s[*pos]. Returns 0, or INCOMPLETE with a message.
static int scan_word(const char *s, size_t *pos, char *err, size_t errsz) {
    size_t i = *pos;
    while (s[i] && !is_blank(s[i]) && s[i] != '\n' && !strchr("|&;<>()", s[i])) {
        if (s[i] == '\\') {
            i += s[i + 1] ? 2 : 1;
        } else if (s[i] == '\'') {
            const char *e = strchr(s + i + 1, '\'');
            if (!e) { set_msg(err, errsz, "unterminated single quote", ""); return INCOMPLETE; }
            i = (size_t)(e - s) + 1;
        } else if (s[i] == '"') {
            i++;
            while (s[i] && s[i] != '"') {
                if (s[i] == '\\' && s[i + 1]) {
                    i += 2;
                } else if (s[i] == '$' && s[i + 1] == '(') {
                    size_t e = skip_subst(s, i);
                    if (!e) { set_msg(err, errsz, "unterminated command substitution", ""); return INCOMPLETE; }
                    i = e;
                } else if (s[i] == '`') {
                    size_t e = skip_backtick(s, i);
                    if (!e) { set_msg(err, errsz, "unterminated backquote", ""); return INCOMPLETE; }
                    i = e;
                } else {
                    i++;
                }
            }
            if (!s[i]) { set_msg(err, errsz, "unterminated double quote", ""); return INCOMPLETE; }
            i++;
        } else if (s[i] == '$' && s[i + 1] == '(') {
            size_t e = skip_subst(s, i);
            if (!e) { set_msg(err, errsz, "unterminated command substitution", ""); return INCOMPLETE; }
            i = e;
        } else if (s[i] == '`') {
            size_t e = skip_backtick(s, i);
            if (!e) { set_msg(err, errsz, "unterminated backquote", ""); return INCOMPLETE; }
            i = e;
        } else {
            i++;
        }
    }
    *pos = i;
    return 0;
}

// Read the here-document bodies owed by the tokens in pend[]; `*pos` is just
// after a newline.
static int read_heredocs(const char *s, size_t *pos, Token *t, const int *pend, int npend, char *err, size_t errsz) {
    size_t i = *pos;
    for (int k = 0; k < npend; ++k) {
        Token *h = &t[pend[k]];
        Buf body = {NULL, 0, 0};
        put(&body, "", 0);
        int found = 0;
        while (s[i]) {
            const char *nl = strchr(s + i, '\n');
            size_t len = nl ? (size_t)(nl - (s + i)) : strlen(s + i);
            const char *line = s + i;
            size_t skip = 0;
            if (h->strip_tabs) while (skip < len && line[skip] == '\t') skip++;
            i += len + (nl ? 1 : 0);
            if (len - skip == strlen(h->text) && !strncmp(line + skip, h->text, len - skip)) {
                found = 1;
                break;
            }
            put(&body, line + skip, len - skip);
            put(&body, "\n", 1);
        }
        if (!found) {
            free(body.p);
            set_msg(err, errsz, "unterminated here-document (no line says ", "");
            if (err && errsz) {
                size_t l = strlen(err);
                snprintf(err + l, errsz - l, "%s)", h->text);
            }
            return INCOMPLETE;
        }
        h->body = body.p;
    }
    *pos = i;
    return 0;
}

static int lex(const char *s, Token **out, int *nout, char *err, size_t errsz) {
    int cap = 32, n = 0, npend = 0;
    int pend[64];
    Token *t = malloc((size_t)cap * sizeof *t);
    size_t i = 0;
    int rc = 0;
    for (;;) {
        while (is_blank(s[i])) i++;
        if (s[i] == '\0') break;
        if (s[i] == '#') {
            while (s[i] && s[i] != '\n') i++;
            continue;
        }
        if (n + 2 > cap) {
            cap *= 2;
            t = realloc(t, (size_t)cap * sizeof *t);
        }
        Token tok;
        memset(&tok, 0, sizeof tok);
        tok.kind = T_WORD;
        tok.start = i;

        if (s[i] == '\n') {
            tok.kind = T_NL;
            i++;
            tok.end = i;
            t[n++] = tok;
            if (npend) {
                rc = read_heredocs(s, &i, t, pend, npend, err, errsz);
                if (rc) goto bad;
                npend = 0;
            }
            continue;
        } else if (s[i] == '2' && s[i + 1] == '>') {
            if (s[i + 2] == '&' && s[i + 3] == '1') { tok.kind = T_ERR2OUT; i += 4; }
            else if (s[i + 2] == '>') { tok.kind = T_ERRAPP; i += 3; }
            else { tok.kind = T_ERRGT; i += 2; }
        } else if ((s[i] == '1' && s[i + 1] == '>' && s[i + 2] == '&' && s[i + 3] == '2') ||
                   (s[i] == '>' && s[i + 1] == '&' && s[i + 2] == '2')) {
            tok.kind = T_OUT2ERR;
            i += s[i] == '1' ? 4 : 3;
        } else if (s[i] == '|') {
            if (s[i + 1] == '|') { tok.kind = T_OR; i += 2; } else { tok.kind = T_PIPE; i++; }
        } else if (s[i] == '&') {
            if (s[i + 1] == '&') { tok.kind = T_AND; i += 2; } else { tok.kind = T_AMP; i++; }
        } else if (s[i] == ';') {
            if (s[i + 1] == ';') { tok.kind = T_DSEMI; i += 2; } else { tok.kind = T_SEMI; i++; }
        } else if (s[i] == '(') {
            tok.kind = T_LPAREN; i++;
        } else if (s[i] == ')') {
            tok.kind = T_RPAREN; i++;
        } else if (s[i] == '<') {
            if (s[i + 1] == '<') {
                tok.kind = T_HEREDOC;
                i += 2;
                if (s[i] == '-') { tok.strip_tabs = 1; i++; }
                while (is_blank(s[i])) i++;
                size_t ws = i;
                rc = scan_word(s, &i, err, errsz);
                if (rc) goto bad;
                if (i == ws) {
                    set_msg(err, errsz, "missing here-document end marker", "");
                    rc = -1;
                    goto bad;
                }
                Buf m = {NULL, 0, 0};
                put(&m, "", 0);
                for (size_t k = ws; k < i; ++k) {
                    char c = s[k];
                    if (c == '\\' || c == '\'' || c == '"') { tok.literal = 1; if (c == '\\' && k + 1 < i) { k++; put(&m, s + k, 1); } continue; }
                    put(&m, s + k, 1);
                }
                tok.text = m.p;
                if (npend == 64) { free(tok.text); set_msg(err, errsz, "too many here-documents", ""); rc = -1; goto bad; }
                pend[npend++] = n;
            } else {
                tok.kind = T_LT; i++;
            }
        } else if (s[i] == '>') {
            if (s[i + 1] == '>') { tok.kind = T_APPEND; i += 2; } else { tok.kind = T_GT; i++; }
        } else {
            size_t start = i;
            rc = scan_word(s, &i, err, errsz);
            if (rc) goto bad;
            tok.text = strndup(s + start, i - start);
        }
        tok.end = i;
        t[n++] = tok;
    }
    if (npend) {
        // The end of the text came before the newline that starts the bodies.
        set_msg(err, errsz, "unterminated here-document", "");
        rc = INCOMPLETE;
        goto bad;
    }
    memset(&t[n], 0, sizeof t[n]);
    t[n].kind = T_END;
    t[n].start = t[n].end = i;
    *out = t;
    *nout = n + 1;
    return 0;
bad:
    free_tokens(t, n);
    return rc;
}

// ---- parser -----------------------------------------------------------------

typedef struct {
    const Token *t;
    int p;
    const char *src;
    char *err;
    size_t errsz;
} P;

static int unexpected(P *ps) {
    const Token *k = &ps->t[ps->p];
    return set_msg(ps->err, ps->errsz, "syntax error near unexpected token ", k->kind == T_WORD ? k->text : tok_name(k->kind));
}

static int incomplete(P *ps, const char *msg) {
    set_msg(ps->err, ps->errsz, msg, "");
    return INCOMPLETE;
}

static void *grow(void *p, int count, size_t size) {
    return realloc(p, (size_t)(count + 1) * size);
}

static void list_free_inner(List *l);

static void command_free(Command *c) {
    for (int i = 0; i < c->nwords; ++i) free(c->words[i]);
    for (int i = 0; i < c->nredirs; ++i) free(c->redirs[i].target);
    free(c->words);
    free(c->redirs);
    free(c->var);
    free(c->src);
    for (struct List **l = (struct List *[]){c->a, c->b, c->c, NULL}; *l; ++l) {
        list_free_inner(*l);
        free(*l);
    }
    for (int i = 0; i < c->narms; ++i) {
        for (int k = 0; k < c->arms[i].npatterns; ++k) free(c->arms[i].patterns[k]);
        free(c->arms[i].patterns);
        if (c->arms[i].body) { list_free_inner(c->arms[i].body); free(c->arms[i].body); }
    }
    free(c->arms);
    memset(c, 0, sizeof *c);
}

static void pipeline_free(Pipeline *pl) {
    for (int i = 0; i < pl->ncmds; ++i) command_free(&pl->cmds[i]);
    free(pl->cmds);
}

static void list_free_inner(List *l) {
    if (!l) return;
    for (int i = 0; i < l->nitems; ++i) pipeline_free(&l->items[i].pipeline);
    free(l->items);
    l->items = NULL;
    l->nitems = 0;
}

void list_free(List *l) { list_free_inner(l); }

static int is_kw(const P *ps, const char *kw) {
    const Token *k = &ps->t[ps->p];
    return k->kind == T_WORD && !strcmp(k->text, kw);
}

static int valid_name_s(const char *s) {
    if (!*s || isdigit((unsigned char)*s)) return 0;
    for (; *s; ++s)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

// Tokens that end a list of commands: the caller decides whether that is right.
static int at_closer(const P *ps) {
    const Token *k = &ps->t[ps->p];
    switch (k->kind) {
        case T_RPAREN: case T_DSEMI: case T_END: return 1;
        case T_WORD: {
            static const char *const kws[] = {"then", "elif", "else", "fi", "do", "done", "esac", "}", NULL};
            for (int i = 0; kws[i]; ++i)
                if (!strcmp(k->text, kws[i])) return 1;
            return 0;
        }
        default: return 0;
    }
}

static int parse_list_into(P *ps, List **dst);
static int parse_command(P *ps, Command *c);

static void skip_newlines(P *ps) {
    while (ps->t[ps->p].kind == T_NL) ps->p++;
}

// A list that must hold at least one command.
static int parse_body(P *ps, List **dst) {
    int rc = parse_list_into(ps, dst);
    if (rc) return rc;
    if ((*dst)->nitems == 0) {
        if (ps->t[ps->p].kind == T_END) return incomplete(ps, "syntax error: unexpected end of input");
        return unexpected(ps);
    }
    return 0;
}

static int expect_kw(P *ps, const char *kw) {
    if (is_kw(ps, kw)) { ps->p++; return 0; }
    if (ps->t[ps->p].kind == T_END) {
        char msg[96];
        snprintf(msg, sizeof msg, "syntax error: unexpected end of input (missing `%s')", kw);
        return incomplete(ps, msg);
    }
    return unexpected(ps);
}

// One redirection if the next token is one. Returns 1 when it consumed one,
// 0 when the next token is not a redirection, -1 on error.
static int parse_redir(P *ps, Command *c) {
    const Token *k = &ps->t[ps->p];
    if (k->kind == T_ERR2OUT || k->kind == T_OUT2ERR) {
        c->redirs = grow(c->redirs, c->nredirs, sizeof *c->redirs);
        Redir r = {k->kind == T_ERR2OUT ? R_ERR_TO_OUT : R_OUT_TO_ERR, NULL, 0};
        c->redirs[c->nredirs++] = r;
        ps->p++;
        return 1;
    }
    if (k->kind == T_HEREDOC) {
        c->redirs = grow(c->redirs, c->nredirs, sizeof *c->redirs);
        Redir r = {R_HEREDOC, strdup(k->body ? k->body : ""), k->literal};
        c->redirs[c->nredirs++] = r;
        ps->p++;
        return 1;
    }
    if (k->kind == T_LT || k->kind == T_GT || k->kind == T_APPEND || k->kind == T_ERRGT || k->kind == T_ERRAPP) {
        if (ps->t[ps->p + 1].kind != T_WORD)
            return set_msg(ps->err, ps->errsz, "missing file name after ", tok_name(k->kind));
        c->redirs = grow(c->redirs, c->nredirs, sizeof *c->redirs);
        Redir r;
        r.kind = k->kind == T_LT ? R_IN : k->kind == T_GT ? R_OUT : k->kind == T_APPEND ? R_APPEND : k->kind == T_ERRAPP ? R_ERR_APPEND : R_ERR;
        r.target = strdup(ps->t[ps->p + 1].text);
        r.literal = 0;
        c->redirs[c->nredirs++] = r;
        ps->p += 2;
        return 1;
    }
    return 0;
}

static int parse_simple(P *ps, Command *c) {
    c->kind = C_SIMPLE;
    for (;;) {
        TokKind k = ps->t[ps->p].kind;
        if (k == T_WORD) {
            c->words = grow(c->words, c->nwords, sizeof *c->words);
            c->words[c->nwords++] = strdup(ps->t[ps->p].text);
            ps->p++;
            continue;
        }
        int r = parse_redir(ps, c);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    if (c->nwords == 0 && c->nredirs == 0) return unexpected(ps);
    return 0;
}

static int parse_if(P *ps, Command *c, int is_elif) {
    (void)is_elif;
    ps->p++;  // `if` or `elif`
    c->kind = C_IF;
    int rc = parse_body(ps, &c->a);
    if (rc) return rc;
    if ((rc = expect_kw(ps, "then")) != 0) return rc;
    if ((rc = parse_body(ps, &c->b)) != 0) return rc;
    if (is_kw(ps, "elif")) {
        Command inner;
        memset(&inner, 0, sizeof inner);
        if ((rc = parse_if(ps, &inner, 1)) != 0) { command_free(&inner); return rc; }
        c->c = calloc(1, sizeof *c->c);
        c->c->items = calloc(1, sizeof *c->c->items);
        c->c->nitems = 1;
        c->c->items[0].pipeline.cmds = malloc(sizeof(Command));
        c->c->items[0].pipeline.cmds[0] = inner;
        c->c->items[0].pipeline.ncmds = 1;
        c->c->items[0].link = LINK_SEQ;
        return 0;  // the inner `if` consumed the `fi`
    }
    if (is_kw(ps, "else")) {
        ps->p++;
        if ((rc = parse_body(ps, &c->c)) != 0) return rc;
    }
    return expect_kw(ps, "fi");
}

static int parse_while(P *ps, Command *c) {
    c->kind = C_WHILE;
    c->until = is_kw(ps, "until");
    ps->p++;
    int rc = parse_body(ps, &c->a);
    if (rc) return rc;
    if ((rc = expect_kw(ps, "do")) != 0) return rc;
    if ((rc = parse_body(ps, &c->b)) != 0) return rc;
    return expect_kw(ps, "done");
}

static int parse_for(P *ps, Command *c) {
    c->kind = C_FOR;
    ps->p++;
    const Token *name = &ps->t[ps->p];
    if (name->kind == T_END) return incomplete(ps, "syntax error: unexpected end of input (missing loop variable)");
    if (name->kind != T_WORD || !valid_name_s(name->text)) return unexpected(ps);
    c->var = strdup(name->text);
    ps->p++;
    skip_newlines(ps);
    if (is_kw(ps, "in")) {
        c->has_in = 1;
        ps->p++;
        while (ps->t[ps->p].kind == T_WORD) {
            c->words = grow(c->words, c->nwords, sizeof *c->words);
            c->words[c->nwords++] = strdup(ps->t[ps->p].text);
            ps->p++;
        }
    }
    if (ps->t[ps->p].kind == T_SEMI || ps->t[ps->p].kind == T_NL) {
        while (ps->t[ps->p].kind == T_SEMI || ps->t[ps->p].kind == T_NL) ps->p++;
    } else if (ps->t[ps->p].kind == T_END) {
        return incomplete(ps, "syntax error: unexpected end of input (missing `do')");
    } else {
        return unexpected(ps);
    }
    int rc;
    if ((rc = expect_kw(ps, "do")) != 0) return rc;
    if ((rc = parse_body(ps, &c->a)) != 0) return rc;
    return expect_kw(ps, "done");
}

static int parse_case(P *ps, Command *c) {
    c->kind = C_CASE;
    ps->p++;
    if (ps->t[ps->p].kind == T_END) return incomplete(ps, "syntax error: unexpected end of input (missing `esac')");
    if (ps->t[ps->p].kind != T_WORD) return unexpected(ps);
    c->words = grow(c->words, c->nwords, sizeof *c->words);
    c->words[c->nwords++] = strdup(ps->t[ps->p].text);
    ps->p++;
    skip_newlines(ps);
    int rc = expect_kw(ps, "in");
    if (rc) return rc;
    skip_newlines(ps);
    while (!is_kw(ps, "esac")) {
        if (ps->t[ps->p].kind == T_END) return incomplete(ps, "syntax error: unexpected end of input (missing `esac')");
        CaseArm arm;
        memset(&arm, 0, sizeof arm);
        if (ps->t[ps->p].kind == T_LPAREN) ps->p++;
        for (;;) {
            if (ps->t[ps->p].kind == T_END) { rc = incomplete(ps, "syntax error: unexpected end of input (missing `esac')"); goto arm_bad; }
            if (ps->t[ps->p].kind != T_WORD) { rc = unexpected(ps); goto arm_bad; }
            arm.patterns = grow(arm.patterns, arm.npatterns, sizeof *arm.patterns);
            arm.patterns[arm.npatterns++] = strdup(ps->t[ps->p].text);
            ps->p++;
            if (ps->t[ps->p].kind == T_PIPE) { ps->p++; continue; }
            break;
        }
        if (ps->t[ps->p].kind != T_RPAREN) {
            rc = ps->t[ps->p].kind == T_END ? incomplete(ps, "syntax error: unexpected end of input (missing `)')") : unexpected(ps);
            goto arm_bad;
        }
        ps->p++;
        if ((rc = parse_list_into(ps, &arm.body)) != 0) goto arm_bad;
        c->arms = grow(c->arms, c->narms, sizeof *c->arms);
        c->arms[c->narms++] = arm;
        if (ps->t[ps->p].kind == T_DSEMI) ps->p++;
        else if (!is_kw(ps, "esac")) return ps->t[ps->p].kind == T_END ? incomplete(ps, "syntax error: unexpected end of input (missing `esac')") : unexpected(ps);
        skip_newlines(ps);
        continue;
    arm_bad:
        for (int k = 0; k < arm.npatterns; ++k) free(arm.patterns[k]);
        free(arm.patterns);
        if (arm.body) { list_free_inner(arm.body); free(arm.body); }
        return rc;
    }
    ps->p++;  // esac
    return 0;
}

static int parse_func(P *ps, Command *c, int has_keyword) {
    const Token *first = &ps->t[ps->p];
    if (has_keyword) ps->p++;
    const Token *name = &ps->t[ps->p];
    if (name->kind != T_WORD || !valid_name_s(name->text)) return name->kind == T_END ? incomplete(ps, "syntax error: unexpected end of input") : unexpected(ps);
    ps->p++;
    if (ps->t[ps->p].kind == T_LPAREN) {
        ps->p++;
        if (ps->t[ps->p].kind != T_RPAREN) return unexpected(ps);
        ps->p++;
    } else if (!has_keyword) {
        return unexpected(ps);
    }
    skip_newlines(ps);
    (void)first;
    size_t start = ps->t[ps->p].start;
    if (ps->t[ps->p].kind == T_END) return incomplete(ps, "syntax error: unexpected end of input (missing function body)");
    Command body;
    memset(&body, 0, sizeof body);
    int rc = parse_command(ps, &body);
    if (rc) { command_free(&body); return rc; }
    if (body.kind == C_SIMPLE) {
        command_free(&body);
        return set_msg(ps->err, ps->errsz, "syntax error: a function body must be { ...; } or ( ... )", "");
    }
    size_t end = ps->t[ps->p - 1].end;
    command_free(&body);
    c->kind = C_FUNC;
    c->words = grow(NULL, 0, sizeof *c->words);
    c->words[0] = strdup(name->text);
    c->nwords = 1;
    c->src = strndup(ps->src + start, end - start);
    return 0;
}

static int parse_command(P *ps, Command *c) {
    memset(c, 0, sizeof *c);
    const Token *k = &ps->t[ps->p];
    int rc;
    if (k->kind == T_LPAREN) {
        ps->p++;
        c->kind = C_SUBSHELL;
        if ((rc = parse_body(ps, &c->a)) != 0) return rc;
        if (ps->t[ps->p].kind == T_RPAREN) ps->p++;
        else if (ps->t[ps->p].kind == T_END) return incomplete(ps, "syntax error: unexpected end of input (missing `)')");
        else return unexpected(ps);
    } else if (k->kind == T_WORD && !strcmp(k->text, "if")) {
        if ((rc = parse_if(ps, c, 0)) != 0) return rc;
    } else if (k->kind == T_WORD && (!strcmp(k->text, "while") || !strcmp(k->text, "until"))) {
        if ((rc = parse_while(ps, c)) != 0) return rc;
    } else if (k->kind == T_WORD && !strcmp(k->text, "for")) {
        if ((rc = parse_for(ps, c)) != 0) return rc;
    } else if (k->kind == T_WORD && !strcmp(k->text, "case")) {
        if ((rc = parse_case(ps, c)) != 0) return rc;
    } else if (k->kind == T_WORD && !strcmp(k->text, "{")) {
        ps->p++;
        c->kind = C_GROUP;
        if ((rc = parse_body(ps, &c->a)) != 0) return rc;
        if ((rc = expect_kw(ps, "}")) != 0) return rc;
    } else if (k->kind == T_WORD && !strcmp(k->text, "function")) {
        if ((rc = parse_func(ps, c, 1)) != 0) return rc;
        return 0;
    } else if (k->kind == T_WORD && ps->t[ps->p + 1].kind == T_LPAREN && ps->t[ps->p + 2].kind == T_RPAREN &&
               valid_name_s(k->text)) {
        if ((rc = parse_func(ps, c, 0)) != 0) return rc;
        return 0;
    } else {
        return parse_simple(ps, c);
    }
    // redirections after a compound command: `done < file`, `} > log`
    for (;;) {
        int r = parse_redir(ps, c);
        if (r < 0) return -1;
        if (r == 0) break;
    }
    return 0;
}

static int parse_pipeline(P *ps, Pipeline *pl) {
    memset(pl, 0, sizeof *pl);
    if (is_kw(ps, "!")) {
        pl->negate = 1;
        ps->p++;
    }
    for (;;) {
        Command c;
        int rc = parse_command(ps, &c);
        if (rc) {
            command_free(&c);
            pipeline_free(pl);
            return rc;
        }
        pl->cmds = grow(pl->cmds, pl->ncmds, sizeof *pl->cmds);
        pl->cmds[pl->ncmds++] = c;
        if (ps->t[ps->p].kind != T_PIPE) return 0;
        ps->p++;
        skip_newlines(ps);
        if (ps->t[ps->p].kind == T_END) {
            pipeline_free(pl);
            return incomplete(ps, "syntax error near unexpected token newline");
        }
    }
}

static int parse_list_into(P *ps, List **dst) {
    List *l = calloc(1, sizeof *l);
    *dst = l;
    for (;;) {
        skip_newlines(ps);
        if (at_closer(ps)) return 0;
        Item it;
        memset(&it, 0, sizeof it);
        int rc = parse_pipeline(ps, &it.pipeline);
        if (rc) return rc;
        TokKind k = ps->t[ps->p].kind;
        it.link = LINK_SEQ;
        if (k == T_AND || k == T_OR) {
            it.link = k == T_AND ? LINK_AND : LINK_OR;
            ps->p++;
            skip_newlines(ps);
            if (ps->t[ps->p].kind == T_END) {
                pipeline_free(&it.pipeline);
                char msg[64];
                snprintf(msg, sizeof msg, "syntax error: nothing after %s", tok_name(k));
                return incomplete(ps, msg);
            }
            if (at_closer(ps)) {
                pipeline_free(&it.pipeline);
                return unexpected(ps);
            }
        } else if (k == T_SEMI) {
            ps->p++;
        } else if (k == T_AMP) {
            it.link = LINK_BG;
            ps->p++;
        } else if (k != T_NL && !at_closer(ps)) {
            pipeline_free(&it.pipeline);
            return unexpected(ps);
        }
        l->items = grow(l->items, l->nitems, sizeof *l->items);
        l->items[l->nitems++] = it;
    }
}

int parse_script(const char *text, List *out, char *err, size_t errsz) {
    out->items = NULL;
    out->nitems = 0;
    size_t len = strlen(text);
    // A backslash at the very end of the text continues onto a line that has not arrived.
    if (len >= 2 && text[len - 1] == '\n' && text[len - 2] == '\\') {
        size_t bs = 0;
        for (size_t k = len - 1; k > 0 && text[k - 1] == '\\'; --k) bs++;
        if (bs % 2 == 1) { set_msg(err, errsz, "unexpected end of input after a backslash", ""); return 1; }
    }
    // Remove backslash-newline pairs (line continuation).
    Buf clean = {NULL, 0, 0};
    put(&clean, "", 0);
    for (size_t i = 0; i < len; ++i) {
        if (text[i] == '\\' && text[i + 1] == '\n') { i++; continue; }
        if (text[i] == '\\' && text[i + 1]) { put(&clean, text + i, 2); i++; continue; }
        put(&clean, text + i, 1);
    }

    Token *t;
    int nt;
    int rc = lex(clean.p, &t, &nt, err, errsz);
    if (rc) { free(clean.p); return rc == INCOMPLETE ? 1 : -1; }

    P ps = {t, 0, clean.p, err, errsz};
    List *l = NULL;
    rc = parse_list_into(&ps, &l);
    if (rc == 0 && t[ps.p].kind != T_END) rc = unexpected(&ps);
    if (rc) {
        list_free_inner(l);
        free(l);
    } else {
        *out = *l;
        free(l);
    }
    free_tokens(t, nt);
    free(clean.p);
    return rc == INCOMPLETE ? 1 : rc;
}

int parse_line(const char *line, List *out, char *err, size_t errsz) {
    return parse_script(line, out, err, errsz) == 0 ? 0 : -1;
}

// ---- expansion --------------------------------------------------------------

enum { F_PLAIN = 0, F_QUOTED = 1, F_EXP = 2, F_BREAK = 4, F_MARK = 8 };

typedef struct {
    unsigned char c, fl;
} Cell;

typedef struct {
    Cell *p;
    size_t n, cap;
} Cells;

static void cput(Cells *cs, char c, int fl) {
    if (cs->n == cs->cap) {
        cs->cap = cs->cap ? cs->cap * 2 : 32;
        cs->p = realloc(cs->p, cs->cap * sizeof *cs->p);
    }
    cs->p[cs->n].c = (unsigned char)c;
    cs->p[cs->n].fl = (unsigned char)fl;
    cs->n++;
}

static void cputs(Cells *cs, const char *s, int fl) {
    for (; *s; ++s) cput(cs, *s, fl);
}

static int name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// ---- arithmetic: $((...)) ----

typedef struct {
    const char *s;
    size_t i;
    const ExpandEnv *env;
    char *err;
    size_t errsz;
    int failed;
} Ar;

static void ar_fail(Ar *a, const char *msg) {
    if (!a->failed) set_msg(a->err, a->errsz, msg, "");
    a->failed = 1;
}

static void ar_ws(Ar *a) {
    while (a->s[a->i] == ' ' || a->s[a->i] == '\t' || a->s[a->i] == '\n') a->i++;
}

static long ar_or(Ar *a);

static long ar_primary(Ar *a) {
    ar_ws(a);
    char c = a->s[a->i];
    if (c == '(') {
        a->i++;
        long v = ar_or(a);
        ar_ws(a);
        if (a->s[a->i] != ')') ar_fail(a, "arithmetic: missing )");
        else a->i++;
        return v;
    }
    if (c == '-' || c == '+' || c == '!' || c == '~') {
        a->i++;
        long v = ar_primary(a);
        return c == '-' ? -v : c == '!' ? !v : c == '~' ? ~v : v;
    }
    if (isdigit((unsigned char)c)) {
        char *end;
        long v = strtol(a->s + a->i, &end, 0);
        a->i = (size_t)(end - a->s);
        return v;
    }
    if (name_char(c)) {
        char name[128];
        size_t n = 0;
        while (name_char(a->s[a->i]) && n < sizeof name - 1) name[n++] = a->s[a->i++];
        name[n] = '\0';
        const char *v = a->env->lookup(name, a->env->ctx);
        return v ? strtol(v, NULL, 0) : 0;
    }
    ar_fail(a, "arithmetic: syntax error in expression");
    return 0;
}

static long ar_mul(Ar *a) {
    long v = ar_primary(a);
    for (;;) {
        ar_ws(a);
        char c = a->s[a->i];
        if (c != '*' && c != '/' && c != '%') return v;
        a->i++;
        long r = ar_primary(a);
        if (c == '*') v *= r;
        else if (r == 0) { ar_fail(a, "arithmetic: division by zero"); return 0; }
        else v = c == '/' ? v / r : v % r;
    }
}

static long ar_add(Ar *a) {
    long v = ar_mul(a);
    for (;;) {
        ar_ws(a);
        char c = a->s[a->i];
        if (c != '+' && c != '-') return v;
        a->i++;
        long r = ar_mul(a);
        v = c == '+' ? v + r : v - r;
    }
}

static long ar_cmp(Ar *a) {
    long v = ar_add(a);
    for (;;) {
        ar_ws(a);
        const char *p = a->s + a->i;
        int op = 0;
        if (!strncmp(p, "<=", 2)) op = 1;
        else if (!strncmp(p, ">=", 2)) op = 2;
        else if (!strncmp(p, "==", 2)) op = 3;
        else if (!strncmp(p, "!=", 2)) op = 4;
        else if (*p == '<' ) op = 5;
        else if (*p == '>') op = 6;
        if (!op) return v;
        a->i += op <= 4 ? 2 : 1;
        long r = ar_add(a);
        switch (op) {
            case 1: v = v <= r; break;
            case 2: v = v >= r; break;
            case 3: v = v == r; break;
            case 4: v = v != r; break;
            case 5: v = v < r; break;
            default: v = v > r; break;
        }
    }
}

static long ar_and(Ar *a) {
    long v = ar_cmp(a);
    for (;;) {
        ar_ws(a);
        if (strncmp(a->s + a->i, "&&", 2)) return v;
        a->i += 2;
        long r = ar_cmp(a);
        v = v && r;
    }
}

static long ar_or(Ar *a) {
    long v = ar_and(a);
    for (;;) {
        ar_ws(a);
        if (strncmp(a->s + a->i, "||", 2)) return v;
        a->i += 2;
        long r = ar_and(a);
        v = v || r;
    }
}

int eval_arith(const char *expr, const ExpandEnv *env, long *result, char *err, size_t errsz) {
    Ar a = {expr, 0, env, err, errsz, 0};
    long v = ar_or(&a);
    ar_ws(&a);
    if (!a.failed && expr[a.i]) ar_fail(&a, "arithmetic: syntax error in expression");
    if (a.failed) return -1;
    *result = v;
    return 0;
}

// ---- $ expansions ----

static char *expand_join(const Cells *cs);
static void expand_cells(const char *raw, const ExpandEnv *env, Cells *cs);

static void run_subst(Cells *cs, const char *cmd, size_t len, const ExpandEnv *env, int fl) {
    if (!env->cmdsub) {
        cput(cs, '$', fl);
        cput(cs, '(', fl);
        for (size_t i = 0; i < len; ++i) cput(cs, cmd[i], fl);
        cput(cs, ')', fl);
        return;
    }
    char *text = strndup(cmd, len);
    char *out = env->cmdsub(text, env->ctx);
    free(text);
    if (out) cputs(cs, out, fl);
    free(out);
}

// raw[i] is '$'. Appends the value and returns the index after the reference.
static size_t dollar(Cells *cs, const char *raw, size_t i, const ExpandEnv *env, int quoted) {
    int fl = quoted ? F_QUOTED : F_EXP;
    size_t j = i + 1;
    char name[128];

    if (raw[j] == '(') {
        size_t e = skip_subst(raw, i);
        if (!e) { cput(cs, '$', fl); return i + 1; }
        if (raw[j + 1] == '(' && e - i >= 5 && raw[e - 2] == ')') {
            char *inner = strndup(raw + i + 3, e - i - 5);
            Cells ic = {0};
            expand_cells(inner, env, &ic);
            char *expr = expand_join(&ic);
            free(ic.p);
            long v = 0;
            char err[120];
            if (eval_arith(expr, env, &v, err, sizeof err) != 0) {
                fprintf(stderr, "msh: %s\n", err);
                v = 0;
            }
            char num[32];
            snprintf(num, sizeof num, "%ld", v);
            cputs(cs, num, fl);
            free(expr);
            free(inner);
            return e;
        }
        run_subst(cs, raw + i + 2, e - i - 3, env, fl);
        return e;
    }
    if (raw[j] == '@' || raw[j] == '*') {
        char which = raw[j];
        if (quoted && which == '@') {
            const char *cnt = env->lookup("#", env->ctx);
            int n = cnt ? atoi(cnt) : 0;
            for (int k = 1; k <= n; ++k) {
                char nm[16];
                snprintf(nm, sizeof nm, "%d", k);
                const char *v = env->lookup(nm, env->ctx);
                if (k > 1) cput(cs, 0, F_BREAK);
                cput(cs, 0, F_MARK);
                if (v) cputs(cs, v, F_QUOTED);
            }
        } else {
            const char *v = env->lookup("*", env->ctx);
            if (v) cputs(cs, v, fl);
        }
        return j + 1;
    }
    if (raw[j] == '?' || raw[j] == '$' || raw[j] == '#' || isdigit((unsigned char)raw[j])) {
        name[0] = raw[j];
        name[1] = '\0';
        j++;
    } else if (raw[j] == '{') {
        const char *end = strchr(raw + j, '}');
        if (!end || (size_t)(end - raw - j - 1) >= sizeof name) {
            cput(cs, '$', fl);  // not a valid reference: keep it literally
            return i + 1;
        }
        size_t len = (size_t)(end - raw) - j - 1;
        char inner[128];
        memcpy(inner, raw + j + 1, len);
        inner[len] = '\0';
        j = (size_t)(end - raw) + 1;
        if (inner[0] == '#' && len > 1) {  // ${#NAME}: length
            const char *v = env->lookup(inner + 1, env->ctx);
            char num[32];
            snprintf(num, sizeof num, "%zu", v ? strlen(v) : 0);
            cputs(cs, num, fl);
            return j;
        }
        char *dflt = strstr(inner, ":-");
        if (dflt) *dflt = '\0';
        const char *v = env->lookup(inner, env->ctx);
        if (dflt && (!v || !*v)) {
            char *d = expand_word_env(dflt + 2, env);
            cputs(cs, d, fl);
            free(d);
        } else if (v) {
            cputs(cs, v, fl);
        }
        return j;
    } else if (name_char(raw[j])) {
        size_t len = 0;
        while (name_char(raw[j]) && len < sizeof name - 1) name[len++] = raw[j++];
        name[len] = '\0';
    } else {
        cput(cs, '$', fl);
        return i + 1;
    }
    const char *v = env->lookup(name, env->ctx);
    if (v) cputs(cs, v, fl);
    return j;
}

// raw[i] is a backquote.
static size_t backtick(Cells *cs, const char *raw, size_t i, const ExpandEnv *env, int quoted) {
    size_t e = skip_backtick(raw, i);
    if (!e) { cput(cs, '`', quoted ? F_QUOTED : F_EXP); return i + 1; }
    Buf cmd = {NULL, 0, 0};
    put(&cmd, "", 0);
    for (size_t k = i + 1; k < e - 1; ++k) {
        if (raw[k] == '\\' && strchr("`\\$", raw[k + 1])) k++;
        put(&cmd, raw + k, 1);
    }
    run_subst(cs, cmd.p, cmd.n, env, quoted ? F_QUOTED : F_EXP);
    free(cmd.p);
    return e;
}

static void expand_cells(const char *raw, const ExpandEnv *env, Cells *cs) {
    size_t i = 0;
    if (raw[0] == '~' && (raw[1] == '\0' || raw[1] == '/')) {
        const char *home = env->lookup("HOME", env->ctx);
        if (home) {
            cput(cs, 0, F_MARK);
            cputs(cs, home, F_QUOTED);
            i = 1;
        }
    }
    while (raw[i]) {
        char c = raw[i];
        if (c == '\\') {
            if (raw[i + 1]) { cput(cs, raw[i + 1], F_QUOTED); i += 2; }
            else { cput(cs, '\\', F_QUOTED); i++; }
        } else if (c == '\'') {
            const char *e = strchr(raw + i + 1, '\'');
            if (!e) { cput(cs, c, F_QUOTED); i++; continue; }  // the lexer prevents this
            cput(cs, 0, F_MARK);
            for (const char *q = raw + i + 1; q < e; ++q) cput(cs, *q, F_QUOTED);
            i = (size_t)(e - raw) + 1;
        } else if (c == '"') {
            cput(cs, 0, F_MARK);
            i++;
            while (raw[i] && raw[i] != '"') {
                if (raw[i] == '\\' && raw[i + 1] && strchr("\"\\$`", raw[i + 1])) {
                    cput(cs, raw[i + 1], F_QUOTED);
                    i += 2;
                } else if (raw[i] == '$') {
                    i = dollar(cs, raw, i, env, 1);
                } else if (raw[i] == '`') {
                    i = backtick(cs, raw, i, env, 1);
                } else {
                    cput(cs, raw[i], F_QUOTED);
                    i++;
                }
            }
            if (raw[i]) i++;
        } else if (c == '$') {
            i = dollar(cs, raw, i, env, 0);
        } else if (c == '`') {
            i = backtick(cs, raw, i, env, 0);
        } else {
            cput(cs, c, F_PLAIN);
            i++;
        }
    }
}

// All the characters, no splitting. A word break (from "$@") becomes a space.
static char *expand_join(const Cells *cs) {
    char *out = malloc(cs->n + 1);
    size_t n = 0;
    for (size_t i = 0; i < cs->n; ++i) {
        if (cs->p[i].fl & F_MARK) continue;
        out[n++] = (cs->p[i].fl & F_BREAK) ? ' ' : (char)cs->p[i].c;
    }
    out[n] = '\0';
    return out;
}

char *expand_word_env(const char *raw, const ExpandEnv *env) {
    Cells cs = {0};
    expand_cells(raw, env, &cs);
    char *out = expand_join(&cs);
    free(cs.p);
    return out;
}

char *expand_word(const char *raw, LookupFn lookup, void *ctx) {
    ExpandEnv env = {lookup, NULL, ctx};
    return expand_word_env(raw, &env);
}

// A pattern string for glob(3): everything that did not come from an unquoted
// word is escaped. *has_glob says whether an unquoted wildcard is present.
static char *cells_pattern(const Cell *p, size_t n, int *has_glob) {
    Buf b = {NULL, 0, 0};
    put(&b, "", 0);
    *has_glob = 0;
    for (size_t i = 0; i < n; ++i) {
        if (p[i].fl & (F_MARK | F_BREAK)) continue;
        char c = (char)p[i].c;
        if (p[i].fl == F_PLAIN && strchr("*?[", c)) *has_glob = 1;
        if (p[i].fl != F_PLAIN && strchr("*?[]\\", c)) put(&b, "\\", 1);
        put(&b, &c, 1);
    }
    return b.p;
}

char *expand_pattern(const char *raw, LookupFn lookup, void *ctx, int *has_glob) {
    ExpandEnv env = {lookup, NULL, ctx};
    Cells cs = {0};
    expand_cells(raw, &env, &cs);
    char *out = cells_pattern(cs.p, cs.n, has_glob);
    free(cs.p);
    return out;
}

typedef struct {
    char **v;
    int n, cap;
} Fields;

static void fields_add(Fields *f, char *s) {
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 8;
        f->v = realloc(f->v, (size_t)f->cap * sizeof *f->v);
    }
    f->v[f->n++] = s;
}

// One finished word: match wildcards, or keep it as written.
static void emit_field(Fields *f, const Cell *p, size_t n) {
    int has_glob;
    char *pat = cells_pattern(p, n, &has_glob);
    if (has_glob) {
        glob_t g;
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t k = 0; k < g.gl_pathc; ++k) fields_add(f, strdup(g.gl_pathv[k]));
            globfree(&g);
            free(pat);
            return;
        }
    }
    free(pat);
    Cells lit = {0};
    for (size_t i = 0; i < n; ++i)
        if (!(p[i].fl & (F_MARK | F_BREAK))) cput(&lit, (char)p[i].c, F_PLAIN);
    char *s = malloc(lit.n + 1);
    for (size_t i = 0; i < lit.n; ++i) s[i] = (char)lit.p[i].c;
    s[lit.n] = '\0';
    free(lit.p);
    fields_add(f, s);
}

char **expand_fields(const char *raw, const ExpandEnv *env, int *n) {
    Cells cs = {0};
    expand_cells(raw, env, &cs);
    Fields f = {0};
    Cells cur = {0};
    int started = 0;
    for (size_t i = 0; i < cs.n; ++i) {
        Cell c = cs.p[i];
        if (c.fl & F_BREAK) {
            if (started) emit_field(&f, cur.p, cur.n);
            cur.n = 0;
            started = 0;
        } else if (c.fl & F_MARK) {
            started = 1;
        } else if (c.fl == F_EXP && isspace(c.c)) {
            if (started) emit_field(&f, cur.p, cur.n);
            cur.n = 0;
            started = 0;
        } else {
            cput(&cur, (char)c.c, c.fl);
            started = 1;
        }
    }
    if (started) emit_field(&f, cur.p, cur.n);
    free(cur.p);
    free(cs.p);
    *n = f.n;
    if (!f.v) f.v = malloc(sizeof *f.v);
    return f.v;
}

char *expand_heredoc(const char *text, const ExpandEnv *env) {
    Cells cs = {0};
    for (size_t i = 0; text[i];) {
        if (text[i] == '\\' && text[i + 1] && strchr("$`\\", text[i + 1])) {
            cput(&cs, text[i + 1], F_QUOTED);
            i += 2;
        } else if (text[i] == '$') {
            i = dollar(&cs, text, i, env, 1);
        } else if (text[i] == '`') {
            i = backtick(&cs, text, i, env, 1);
        } else {
            cput(&cs, text[i], F_QUOTED);
            i++;
        }
    }
    char *out = expand_join(&cs);
    free(cs.p);
    return out;
}
