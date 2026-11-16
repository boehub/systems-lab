#include "parse.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    T_WORD, T_PIPE, T_AND, T_OR, T_SEMI, T_AMP,
    T_LT, T_GT, T_APPEND, T_ERRGT, T_ERR2OUT, T_END
} TokKind;

typedef struct {
    TokKind kind;
    char *text;  // only for T_WORD
} Token;

static void fail(char *err, size_t n, const char *msg, const char *arg) {
    if (err && n) snprintf(err, n, "%s%s", msg, arg);
}

static const char *tok_name(TokKind k) {
    switch (k) {
        case T_PIPE: return "|";
        case T_AND: return "&&";
        case T_OR: return "||";
        case T_SEMI: return ";";
        case T_AMP: return "&";
        case T_LT: return "<";
        case T_GT: return ">";
        case T_APPEND: return ">>";
        case T_ERRGT: return "2>";
        case T_ERR2OUT: return "2>&1";
        default: return "newline";
    }
}

static void free_tokens(Token *t) {
    for (Token *p = t; p->kind != T_END; ++p) free(p->text);
    free(t);
}

// ---- lexer ----------------------------------------------------------------

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static int lex(const char *s, Token **out, char *err, size_t errsz) {
    int cap = 16, n = 0;
    Token *t = malloc(cap * sizeof *t);
    size_t i = 0;
    for (;;) {
        while (is_space(s[i])) i++;
        if (s[i] == '\0' || s[i] == '#') break;  // end of line, or a comment

        if (n + 2 > cap) {
            cap *= 2;
            t = realloc(t, cap * sizeof *t);
        }
        Token tok = {T_WORD, NULL};

        if (s[i] == '2' && s[i + 1] == '>') {
            if (s[i + 2] == '&' && s[i + 3] == '1') { tok.kind = T_ERR2OUT; i += 4; }
            else { tok.kind = T_ERRGT; i += 2; }
        } else if (s[i] == '|') {
            if (s[i + 1] == '|') { tok.kind = T_OR; i += 2; } else { tok.kind = T_PIPE; i++; }
        } else if (s[i] == '&') {
            if (s[i + 1] == '&') { tok.kind = T_AND; i += 2; } else { tok.kind = T_AMP; i++; }
        } else if (s[i] == ';') {
            tok.kind = T_SEMI; i++;
        } else if (s[i] == '<') {
            tok.kind = T_LT; i++;
        } else if (s[i] == '>') {
            if (s[i + 1] == '>') { tok.kind = T_APPEND; i += 2; } else { tok.kind = T_GT; i++; }
        } else {
            size_t start = i;
            while (s[i] && !is_space(s[i]) && !strchr("|&;<>", s[i])) {
                if (s[i] == '\\') {
                    i += s[i + 1] ? 2 : 1;
                } else if (s[i] == '\'') {
                    const char *e = strchr(s + i + 1, '\'');
                    if (!e) {
                        fail(err, errsz, "unterminated single quote", "");
                        t[n].kind = T_END; t[n].text = NULL;
                        free_tokens(t);
                        return -1;
                    }
                    i = (size_t)(e - s) + 1;
                } else if (s[i] == '"') {
                    i++;
                    while (s[i] && s[i] != '"') i += (s[i] == '\\' && s[i + 1]) ? 2 : 1;
                    if (!s[i]) {
                        fail(err, errsz, "unterminated double quote", "");
                        t[n].kind = T_END; t[n].text = NULL;
                        free_tokens(t);
                        return -1;
                    }
                    i++;
                } else {
                    i++;
                }
            }
            tok.text = strndup(s + start, i - start);
        }
        t[n++] = tok;
    }
    t[n].kind = T_END;
    t[n].text = NULL;
    *out = t;
    return 0;
}

// ---- parser ---------------------------------------------------------------

static void *grow(void *p, int count, size_t size) {
    return realloc(p, (size_t)(count + 1) * size);
}

static void command_free(Command *c);

static int parse_command(const Token *t, int *p, Command *c, char *err, size_t errsz) {
    memset(c, 0, sizeof *c);
    for (;;) {
        TokKind k = t[*p].kind;
        if (k == T_WORD) {
            c->words = grow(c->words, c->nwords, sizeof *c->words);
            c->words[c->nwords++] = strdup(t[*p].text);
            (*p)++;
        } else if (k == T_LT || k == T_GT || k == T_APPEND || k == T_ERRGT) {
            if (t[*p + 1].kind != T_WORD) {
                fail(err, errsz, "missing file name after ", tok_name(k));
                command_free(c);
                return -1;
            }
            c->redirs = grow(c->redirs, c->nredirs, sizeof *c->redirs);
            Redir r;
            r.kind = k == T_LT ? R_IN : k == T_GT ? R_OUT : k == T_APPEND ? R_APPEND : R_ERR;
            r.target = strdup(t[*p + 1].text);
            c->redirs[c->nredirs++] = r;
            *p += 2;
        } else if (k == T_ERR2OUT) {
            c->redirs = grow(c->redirs, c->nredirs, sizeof *c->redirs);
            Redir r = {R_ERR_TO_OUT, NULL};
            c->redirs[c->nredirs++] = r;
            (*p)++;
        } else {
            break;
        }
    }
    if (c->nwords == 0 && c->nredirs == 0) {
        fail(err, errsz, "syntax error near unexpected token ", tok_name(t[*p].kind));
        command_free(c);
        return -1;
    }
    return 0;
}

static void command_free(Command *c) {
    for (int i = 0; i < c->nwords; ++i) free(c->words[i]);
    for (int i = 0; i < c->nredirs; ++i) free(c->redirs[i].target);
    free(c->words);
    free(c->redirs);
}

static void pipeline_free(Pipeline *pl) {
    for (int i = 0; i < pl->ncmds; ++i) command_free(&pl->cmds[i]);
    free(pl->cmds);
}

static int parse_pipeline(const Token *t, int *p, Pipeline *pl, char *err, size_t errsz) {
    memset(pl, 0, sizeof *pl);
    for (;;) {
        Command c;
        if (parse_command(t, p, &c, err, errsz) != 0) {
            pipeline_free(pl);
            return -1;
        }
        pl->cmds = grow(pl->cmds, pl->ncmds, sizeof *pl->cmds);
        pl->cmds[pl->ncmds++] = c;
        if (t[*p].kind != T_PIPE) return 0;
        (*p)++;
    }
}

void list_free(List *l) {
    for (int i = 0; i < l->nitems; ++i) pipeline_free(&l->items[i].pipeline);
    free(l->items);
    l->items = NULL;
    l->nitems = 0;
}

int parse_line(const char *line, List *out, char *err, size_t errsz) {
    out->items = NULL;
    out->nitems = 0;
    Token *t;
    if (lex(line, &t, err, errsz) != 0) return -1;

    int p = 0;
    while (t[p].kind != T_END) {
        Item it;
        if (parse_pipeline(t, &p, &it.pipeline, err, errsz) != 0) goto bad;
        TokKind k = t[p].kind;
        if (k == T_SEMI) { it.link = LINK_SEQ; p++; }
        else if (k == T_AMP) { it.link = LINK_BG; p++; }
        else if (k == T_AND || k == T_OR) {
            it.link = k == T_AND ? LINK_AND : LINK_OR;
            p++;
            if (t[p].kind == T_END) {
                fail(err, errsz, "syntax error: nothing after ", tok_name(k));
                pipeline_free(&it.pipeline);
                goto bad;
            }
        } else {
            it.link = LINK_SEQ;  // end of line
        }
        out->items = grow(out->items, out->nitems, sizeof *out->items);
        out->items[out->nitems++] = it;
    }
    free_tokens(t);
    return 0;

bad:
    list_free(out);
    free_tokens(t);
    return -1;
}

// ---- expansion ------------------------------------------------------------

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

static int name_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

// raw[i] is '$'. Appends the value and returns the index after the reference.
static size_t dollar(Buf *b, const char *raw, size_t i, LookupFn lookup, void *ctx) {
    char name[128];
    size_t j = i + 1;
    if (raw[j] == '?' || raw[j] == '$') {
        name[0] = raw[j];
        name[1] = '\0';
        j++;
    } else if (raw[j] == '{') {
        const char *end = strchr(raw + j, '}');
        if (!end || (size_t)(end - raw - j - 1) >= sizeof name) {
            put(b, "$", 1);  // not a valid reference: keep it literally
            return i + 1;
        }
        size_t len = (size_t)(end - raw) - j - 1;
        memcpy(name, raw + j + 1, len);
        name[len] = '\0';
        j = (size_t)(end - raw) + 1;
    } else if (name_char(raw[j]) && !isdigit((unsigned char)raw[j])) {
        size_t len = 0;
        while (name_char(raw[j]) && len < sizeof name - 1) name[len++] = raw[j++];
        name[len] = '\0';
    } else {
        put(b, "$", 1);
        return i + 1;
    }
    const char *v = lookup(name, ctx);
    if (v) put(b, v, strlen(v));
    return j;
}

// Append text so a later glob() sees it literally: backslash-escape pattern characters.
static void put_lit(Buf *b, const char *t, size_t n, int glob_mode) {
    for (size_t k = 0; k < n; ++k) {
        if (glob_mode && strchr("*?[]\\", t[k])) put(b, "\\", 1);
        put(b, t + k, 1);
    }
}

// Escape the pattern characters in b->p from offset `from` (used for $VARIABLE values).
static void escape_from(Buf *b, size_t from) {
    char *tail = strdup(b->p + from);
    b->n = from;
    b->p[from] = '\0';
    put_lit(b, tail, strlen(tail), 1);
    free(tail);
}

static char *expand_impl(const char *raw, LookupFn lookup, void *ctx, int glob_mode, int *has_glob) {
    Buf b = {NULL, 0, 0};
    put(&b, "", 0);
    size_t i = 0;
    if (raw[0] == '~' && (raw[1] == '\0' || raw[1] == '/')) {
        const char *home = lookup("HOME", ctx);
        if (home) {
            put_lit(&b, home, strlen(home), glob_mode);
            i = 1;
        }
    }
    while (raw[i]) {
        char c = raw[i];
        if (c == '\\') {
            if (raw[i + 1]) { put_lit(&b, raw + i + 1, 1, glob_mode); i += 2; }
            else { put(&b, "\\", 1); i++; }
        } else if (c == '\'') {
            const char *e = strchr(raw + i + 1, '\'');
            if (!e) { put(&b, raw + i, 1); i++; continue; }  // lexer prevents this
            put_lit(&b, raw + i + 1, (size_t)(e - raw) - i - 1, glob_mode);
            i = (size_t)(e - raw) + 1;
        } else if (c == '"') {
            i++;
            while (raw[i] && raw[i] != '"') {
                if (raw[i] == '\\' && strchr("\"\\$`", raw[i + 1]) && raw[i + 1]) {
                    put_lit(&b, raw + i + 1, 1, glob_mode);
                    i += 2;
                } else if (raw[i] == '$') {
                    size_t from = b.n;
                    i = dollar(&b, raw, i, lookup, ctx);
                    if (glob_mode) escape_from(&b, from);
                } else {
                    put_lit(&b, raw + i, 1, glob_mode);
                    i++;
                }
            }
            if (raw[i]) i++;
        } else if (c == '$') {
            size_t from = b.n;
            i = dollar(&b, raw, i, lookup, ctx);
            if (glob_mode) escape_from(&b, from);
        } else {
            if (glob_mode && strchr("*?[", c) && has_glob) *has_glob = 1;
            put(&b, raw + i, 1);
            i++;
        }
    }
    return b.p;
}

char *expand_word(const char *raw, LookupFn lookup, void *ctx) {
    return expand_impl(raw, lookup, ctx, 0, NULL);
}

char *expand_pattern(const char *raw, LookupFn lookup, void *ctx, int *has_glob) {
    *has_glob = 0;
    return expand_impl(raw, lookup, ctx, 1, has_glob);
}
