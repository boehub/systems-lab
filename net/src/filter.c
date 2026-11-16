#include "filter.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    N_TRUE, N_AND, N_OR, N_NOT,
    N_TCP, N_UDP, N_ICMP, N_ICMP6, N_IP, N_IP6, N_ARP, N_DNS, N_HTTP,
    N_HOST, N_NET, N_PORT, N_GREATER, N_LESS, N_SYN, N_FIN, N_RST, N_VLAN, N_BADCSUM
} NodeType;

typedef enum { D_ANY, D_SRC, D_DST } Dir;

typedef struct {
    NodeType type;
    int a, b;      // children (indexes), for AND / OR / NOT
    Dir dir;
    Addr addr;
    int prefix;    // bits, for N_NET
    long number;   // port number or length
} Node;

struct Filter {
    Node *nodes;
    int n, cap;
    int root;
};

typedef struct {
    char **tok;
    int n, pos;
    Filter *f;
    char *err;
    size_t errsz;
    int failed;
} Parser;

static void fail(Parser *p, const char *what, const char *near) {
    if (p->failed) return;
    p->failed = 1;
    if (near) snprintf(p->err, p->errsz, "%s '%s'", what, near);
    else snprintf(p->err, p->errsz, "%s", what);
}

static int add(Parser *p, Node n) {
    Filter *f = p->f;
    if (f->n == f->cap) {
        f->cap = f->cap ? f->cap * 2 : 16;
        f->nodes = realloc(f->nodes, (size_t)f->cap * sizeof *f->nodes);
    }
    f->nodes[f->n] = n;
    return f->n++;
}

static const char *peek(Parser *p) { return p->pos < p->n ? p->tok[p->pos] : NULL; }
static int eat(Parser *p, const char *w) {
    const char *t = peek(p);
    if (t && !strcmp(t, w)) { p->pos++; return 1; }
    return 0;
}

static int parse_or(Parser *p);

static int parse_number(Parser *p, const char *t, long max, long *out) {
    char *end;
    long v = strtol(t, &end, 10);
    if (!*t || *end || v < 0) {
        fail(p, "expected a number, got", t);
        return 0;
    }
    if (v > max) {
        char what[80];
        snprintf(what, sizeof what, "number out of range (the most allowed here is %ld):", max);
        fail(p, what, t);
        return 0;
    }
    *out = v;
    return 1;
}

static int parse_atom(Parser *p) {
    const char *t = peek(p);
    if (!t) { fail(p, "the expression ends too early", NULL); return 0; }
    p->pos++;
    Node n;
    memset(&n, 0, sizeof n);
    n.type = N_TRUE;

    if (!strcmp(t, "(")) {
        int e = parse_or(p);
        if (!eat(p, ")")) fail(p, "missing closing parenthesis", NULL);
        return e;
    }
    static const struct { const char *w; NodeType t; } words[] = {
        {"tcp", N_TCP}, {"udp", N_UDP}, {"icmp", N_ICMP}, {"icmp6", N_ICMP6}, {"ip", N_IP}, {"ip6", N_IP6},
        {"arp", N_ARP}, {"dns", N_DNS}, {"http", N_HTTP}, {"syn", N_SYN}, {"fin", N_FIN}, {"rst", N_RST},
        {"vlan", N_VLAN}, {"badcsum", N_BADCSUM}, {NULL, N_TRUE}};
    for (int i = 0; words[i].w; ++i) {
        if (!strcmp(t, words[i].w)) { n.type = words[i].t; return add(p, n); }
    }
    if (!strcmp(t, "greater") || !strcmp(t, "less")) {
        const char *v = peek(p);
        if (!v) { fail(p, "expected a length after", t); return 0; }
        p->pos++;
        n.type = !strcmp(t, "greater") ? N_GREATER : N_LESS;
        if (!parse_number(p, v, 1000000, &n.number)) return 0;
        return add(p, n);
    }
    n.dir = D_ANY;
    if (!strcmp(t, "src")) { n.dir = D_SRC; t = peek(p); p->pos++; }
    else if (!strcmp(t, "dst")) { n.dir = D_DST; t = peek(p); p->pos++; }
    if (!t) { fail(p, "expected host, net or port after src/dst", NULL); return 0; }

    if (!strcmp(t, "host") || !strcmp(t, "net")) {
        const char *v = peek(p);
        if (!v) { fail(p, "expected an address after", t); return 0; }
        p->pos++;
        char buf[80];
        snprintf(buf, sizeof buf, "%s", v);
        char *slash = strchr(buf, '/');
        if (slash) *slash = '\0';
        if (addr_parse(buf, &n.addr) != 0) { fail(p, "not an IP address:", v); return 0; }
        int maxbits = n.addr.v6 ? 128 : 32;
        n.prefix = maxbits;
        if (!strcmp(t, "net")) {
            n.type = N_NET;
            if (slash) {
                long bits;
                if (!parse_number(p, slash + 1, maxbits, &bits)) return 0;
                n.prefix = (int)bits;
            }
        } else {
            if (slash) { fail(p, "'host' takes a single address, not", v); return 0; }
            n.type = N_HOST;
        }
        return add(p, n);
    }
    if (!strcmp(t, "port")) {
        const char *v = peek(p);
        if (!v) { fail(p, "expected a port number after", t); return 0; }
        p->pos++;
        n.type = N_PORT;
        if (!parse_number(p, v, 65535, &n.number)) return 0;
        return add(p, n);
    }
    fail(p, "unknown filter word", t);
    return 0;
}

static int parse_not(Parser *p) {
    if (eat(p, "not") || eat(p, "!")) {
        int a = parse_not(p);
        Node n = {0};
        n.type = N_NOT;
        n.a = a;
        return add(p, n);
    }
    return parse_atom(p);
}

static int parse_and(Parser *p) {
    int left = parse_not(p);
    while (!p->failed && (eat(p, "and") || eat(p, "&&"))) {
        int right = parse_not(p);
        Node n = {0};
        n.type = N_AND;
        n.a = left;
        n.b = right;
        left = add(p, n);
    }
    return left;
}

static int parse_or(Parser *p) {
    int left = parse_and(p);
    while (!p->failed && (eat(p, "or") || eat(p, "||"))) {
        int right = parse_and(p);
        Node n = {0};
        n.type = N_OR;
        n.a = left;
        n.b = right;
        left = add(p, n);
    }
    return left;
}

// Split into words; parentheses and '!' stand alone even when written without spaces.
static char **tokenize(const char *s, int *count) {
    int cap = 16, n = 0;
    char **out = malloc((size_t)cap * sizeof *out);
    for (size_t i = 0; s[i];) {
        if (isspace((unsigned char)s[i])) { i++; continue; }
        size_t j = i;
        if (s[i] == '(' || s[i] == ')' || s[i] == '!') j = i + 1;
        else if ((s[i] == '&' && s[i + 1] == '&') || (s[i] == '|' && s[i + 1] == '|')) j = i + 2;
        else while (s[j] && !isspace((unsigned char)s[j]) && s[j] != '(' && s[j] != ')' && s[j] != '!') j++;
        if (n == cap) { cap *= 2; out = realloc(out, (size_t)cap * sizeof *out); }
        out[n++] = strndup(s + i, j - i);
        i = j;
    }
    *count = n;
    return out;
}

Filter *filter_compile(const char *expr, char *err, size_t errsz) {
    Filter *f = calloc(1, sizeof *f);
    Parser p = {0};
    p.f = f;
    p.err = err;
    p.errsz = errsz;
    p.tok = tokenize(expr ? expr : "", &p.n);
    if (p.n == 0) {
        Node n = {0};
        n.type = N_TRUE;
        f->root = add(&p, n);
    } else {
        f->root = parse_or(&p);
        if (!p.failed && p.pos < p.n) fail(&p, "unexpected", p.tok[p.pos]);
    }
    for (int i = 0; i < p.n; ++i) free(p.tok[i]);
    free(p.tok);
    if (p.failed) {
        filter_free(f);
        return NULL;
    }
    return f;
}

void filter_free(Filter *f) {
    if (!f) return;
    free(f->nodes);
    free(f);
}

static int prefix_match(const Addr *a, const Addr *net, int bits) {
    if (a->v6 != net->v6) return 0;
    int full = bits / 8, rest = bits % 8;
    if (memcmp(a->b, net->b, (size_t)full) != 0) return 0;
    if (rest) {
        uint8_t mask = (uint8_t)(0xFF << (8 - rest));
        if ((a->b[full] & mask) != (net->b[full] & mask)) return 0;
    }
    return 1;
}

static int eval(const Filter *f, int i, const Packet *p) {
    const Node *n = &f->nodes[i];
    int tcpudp = p->l4 == 6 || p->l4 == 17;
    switch (n->type) {
        case N_TRUE: return 1;
        case N_AND: return eval(f, n->a, p) && eval(f, n->b, p);
        case N_OR: return eval(f, n->a, p) || eval(f, n->b, p);
        case N_NOT: return !eval(f, n->a, p);
        case N_TCP: return p->l4 == 6;
        case N_UDP: return p->l4 == 17;
        case N_ICMP: return p->l4 == 1;
        case N_ICMP6: return p->l4 == 58;
        case N_IP: return p->ip_version == 4;
        case N_IP6: return p->ip_version == 6;
        case N_ARP: return p->has_arp;
        case N_DNS: return p->dns.present;
        case N_HTTP: return p->http != 0;
        case N_SYN: return p->l4 == 6 && (p->tcp_flags & TCP_SYN) && !(p->tcp_flags & TCP_ACK);
        case N_FIN: return p->l4 == 6 && (p->tcp_flags & TCP_FIN);
        case N_RST: return p->l4 == 6 && (p->tcp_flags & TCP_RST);
        case N_VLAN: return p->vlan >= 0;
        case N_BADCSUM: return p->ip_csum == CSUM_BAD || p->l4_csum == CSUM_BAD;
        case N_GREATER: return (long)p->origlen >= n->number;
        case N_LESS: return (long)p->origlen <= n->number;
        case N_PORT:
            if (!tcpudp) return 0;
            if (n->dir == D_SRC) return p->sport == n->number;
            if (n->dir == D_DST) return p->dport == n->number;
            return p->sport == n->number || p->dport == n->number;
        case N_HOST:
        case N_NET: {
            if (!p->ip_version) {
                if (p->has_arp && !n->addr.v6) {  // ARP carries IPv4 addresses too
                    Addr s = {{0}, 0}, d = {{0}, 0};
                    memcpy(s.b, p->arp_spa, 4);
                    memcpy(d.b, p->arp_tpa, 4);
                    if (n->dir != D_DST && prefix_match(&s, &n->addr, n->prefix)) return 1;
                    if (n->dir != D_SRC && prefix_match(&d, &n->addr, n->prefix)) return 1;
                }
                return 0;
            }
            int bits = n->type == N_HOST ? (n->addr.v6 ? 128 : 32) : n->prefix;
            if (n->dir != D_DST && prefix_match(&p->src, &n->addr, bits)) return 1;
            if (n->dir != D_SRC && prefix_match(&p->dst, &n->addr, bits)) return 1;
            return 0;
        }
    }
    return 0;
}

int filter_match(const Filter *f, const Packet *p) { return eval(f, f->root, p); }
