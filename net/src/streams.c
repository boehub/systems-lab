#include "streams.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define MAX_STREAM_BYTES (32u * 1024 * 1024)  // kept per direction; later bytes are counted, not stored
#define OOO_WINDOW (1u << 20)                 // how far ahead of the expected byte a segment may start
#define OOO_MAX_BYTES (4u * 1024 * 1024)      // out-of-order data held per direction

typedef struct Ooo {
    struct Ooo *next;
    uint32_t seq, len;
    uint8_t *data;
} Ooo;

typedef struct {
    uint64_t off, missing;
} Gap;

typedef struct {
    uint8_t dir;
    uint8_t is_gap;
    uint64_t off, len;
} Chunk;

typedef struct {
    int have_base;
    uint32_t next_seq;
    uint8_t *buf;
    size_t len, cap;
    uint64_t skipped;  // bytes past the storage limit
    Gap *gaps;
    int ngaps;
    Ooo *ooo;
    size_t ooo_bytes;
    uint64_t pkts, retrans, ooo_pkts, dup_bytes, wild;
    uint8_t fin;
    uint32_t isn;  // sequence number of the SYN
    int saw_syn;
} Half;

typedef struct {
    Addr client, server;
    uint16_t cport, sport;
    Half h[2];  // [0] client to server, [1] server to client
    int syn, synack, rst;
    Chunk *chunks;
    size_t nchunks, cchunks;
    int next_in_bucket;
} Stream;

struct StreamTable {
    Stream *s;
    size_t n, cap;
    int *buckets;
    size_t nbuckets;
};

StreamTable *streams_new(void) {
    StreamTable *t = calloc(1, sizeof *t);
    t->nbuckets = 1024;
    t->buckets = malloc(t->nbuckets * sizeof *t->buckets);
    for (size_t i = 0; i < t->nbuckets; ++i) t->buckets[i] = -1;
    return t;
}

static void half_free(Half *h) {
    free(h->buf);
    free(h->gaps);
    while (h->ooo) {
        Ooo *o = h->ooo;
        h->ooo = o->next;
        free(o->data);
        free(o);
    }
}

void streams_free(StreamTable *t) {
    if (!t) return;
    for (size_t i = 0; i < t->n; ++i) {
        half_free(&t->s[i].h[0]);
        half_free(&t->s[i].h[1]);
        free(t->s[i].chunks);
    }
    free(t->s);
    free(t->buckets);
    free(t);
}

size_t streams_count(const StreamTable *t) { return t->n; }

// ---- finding the stream a packet belongs to -------------------------------------

static uint32_t hash_end(const Addr *a, uint16_t port) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) h = (h ^ a->b[i]) * 16777619u;
    h = (h ^ port) * 16777619u;
    return h;
}

static int same_end(const Addr *a, uint16_t pa, const Addr *b, uint16_t pb) {
    return pa == pb && a->v6 == b->v6 && !memcmp(a->b, b->b, 16);
}

static Stream *find_stream(StreamTable *t, const Packet *p, int *dir) {
    // the hash must not depend on which side is which, so combine the two ends symmetrically
    uint32_t h = hash_end(&p->src, p->sport) + hash_end(&p->dst, p->dport);
    size_t b = h % t->nbuckets;
    for (int i = t->buckets[b]; i >= 0; i = t->s[i].next_in_bucket) {  // newest first
        Stream *s = &t->s[i];
        if (same_end(&s->client, s->cport, &p->src, p->sport) && same_end(&s->server, s->sport, &p->dst, p->dport)) {
            *dir = 0;
            return s;
        }
        if (same_end(&s->server, s->sport, &p->src, p->sport) && same_end(&s->client, s->cport, &p->dst, p->dport)) {
            *dir = 1;
            return s;
        }
    }
    return NULL;
}

static Stream *add_stream(StreamTable *t, const Packet *p, int *dir) {
    uint32_t h = hash_end(&p->src, p->sport) + hash_end(&p->dst, p->dport);
    size_t b = h % t->nbuckets;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->s = realloc(t->s, t->cap * sizeof *t->s);
    }
    Stream *s = &t->s[t->n];
    memset(s, 0, sizeof *s);
    // The side that sends the SYN is the client; if we join in the middle, the first sender is.
    int reply = (p->tcp_flags & TCP_SYN) && (p->tcp_flags & TCP_ACK);
    s->client = reply ? p->dst : p->src;
    s->cport = reply ? p->dport : p->sport;
    s->server = reply ? p->src : p->dst;
    s->sport = reply ? p->sport : p->dport;
    *dir = reply ? 1 : 0;
    s->next_in_bucket = t->buckets[b];
    t->buckets[b] = (int)t->n;
    t->n++;
    return s;
}

// A fresh SYN on addresses and ports that carried an earlier connection starts a new one.
static int starts_new_connection(const Stream *s, int dir, const Packet *p) {
    if (!(p->tcp_flags & TCP_SYN) || (p->tcp_flags & TCP_ACK) || dir != 0) return 0;
    if (s->rst || (s->h[0].fin && s->h[1].fin)) return 1;
    return s->h[0].saw_syn && s->h[0].isn != p->seq;
}

// ---- delivering bytes ---------------------------------------------------------------

static void add_chunk(Stream *s, int dir, int is_gap, uint64_t off, uint64_t len) {
    if (!is_gap && s->nchunks) {
        Chunk *last = &s->chunks[s->nchunks - 1];
        if (last->dir == dir && !last->is_gap && last->off + last->len == off) {
            last->len += len;
            return;
        }
    }
    if (s->nchunks == s->cchunks) {
        s->cchunks = s->cchunks ? s->cchunks * 2 : 16;
        s->chunks = realloc(s->chunks, s->cchunks * sizeof *s->chunks);
    }
    Chunk c = {(uint8_t)dir, (uint8_t)is_gap, off, len};
    s->chunks[s->nchunks++] = c;
}

static void deliver(Stream *s, int dir, const uint8_t *data, uint32_t n) {
    Half *h = &s->h[dir];
    uint32_t store = n;
    if (h->len + store > MAX_STREAM_BYTES) store = h->len >= MAX_STREAM_BYTES ? 0 : (uint32_t)(MAX_STREAM_BYTES - h->len);
    if (store) {
        if (h->len + store + 1 > h->cap) {
            h->cap = (h->len + store) * 2 + 1;
            h->buf = realloc(h->buf, h->cap);
        }
        memcpy(h->buf + h->len, data, store);
        h->buf[h->len + store] = '\0';  // the HTTP reader parses numbers straight out of the buffer
        add_chunk(s, dir, 0, h->len, store);
        h->len += store;
    }
    h->skipped += n - store;
    h->next_seq += n;
}

static void add_gap(Stream *s, int dir, uint32_t missing) {
    Half *h = &s->h[dir];
    h->gaps = realloc(h->gaps, (size_t)(h->ngaps + 1) * sizeof *h->gaps);
    h->gaps[h->ngaps].off = h->len;
    h->gaps[h->ngaps].missing = missing;
    h->ngaps++;
    add_chunk(s, dir, 1, h->len, missing);
    h->next_seq += missing;
}

// Deliver whatever buffered segments now fit.
static void drain(Stream *s, int dir) {
    Half *h = &s->h[dir];
    while (h->ooo && (int32_t)(h->ooo->seq - h->next_seq) <= 0) {
        Ooo *o = h->ooo;
        h->ooo = o->next;
        h->ooo_bytes -= o->len;
        int32_t skip = (int32_t)(h->next_seq - o->seq);
        if ((uint32_t)skip < o->len) {
            h->dup_bytes += (uint32_t)skip;
            deliver(s, dir, o->data + skip, o->len - (uint32_t)skip);
        } else {
            h->dup_bytes += o->len;
        }
        free(o->data);
        free(o);
    }
}

static void segment(Stream *s, int dir, uint32_t seq, const uint8_t *data, uint32_t len) {
    Half *h = &s->h[dir];
    int32_t delta = (int32_t)(seq - h->next_seq);
    if (delta < 0) {
        uint32_t old = (uint32_t)(-(int64_t)delta);
        if (old >= len) {  // everything here was already delivered
            h->retrans++;
            h->dup_bytes += len;
            return;
        }
        h->dup_bytes += old;
        data += old;
        len -= old;
        delta = 0;
    }
    if (delta == 0) {
        deliver(s, dir, data, len);
        drain(s, dir);
        return;
    }
    if ((uint32_t)delta > OOO_WINDOW || h->ooo_bytes + len > OOO_MAX_BYTES) {
        h->wild++;  // too far ahead to be this connection's next data (or too much to hold)
        return;
    }
    // Hold it until the bytes before it arrive. Keep the list sorted; ignore exact repeats.
    Ooo **pp = &h->ooo;
    while (*pp && (int32_t)((*pp)->seq - h->next_seq) < delta) pp = &(*pp)->next;
    if (*pp && (*pp)->seq == seq && (*pp)->len >= len) {
        h->retrans++;
        return;
    }
    Ooo *o = malloc(sizeof *o);
    o->seq = seq;
    o->len = len;
    o->data = malloc(len);
    memcpy(o->data, data, len);
    o->next = *pp;
    *pp = o;
    h->ooo_bytes += len;
    h->ooo_pkts++;
}

void streams_add(StreamTable *t, const Packet *p) {
    if (p->l4 != 6 || p->fragment || !p->ip_version) return;
    int dir;
    Stream *s = find_stream(t, p, &dir);
    if (!s || starts_new_connection(s, dir, p)) s = add_stream(t, p, &dir);
    Half *h = &s->h[dir];
    h->pkts++;
    uint8_t f = p->tcp_flags;
    if (f & TCP_RST) s->rst = 1;
    if (f & TCP_SYN) {
        if (f & TCP_ACK) s->synack = 1;
        else s->syn = 1;
        if (!h->have_base) {
            h->have_base = 1;
            h->next_seq = p->seq + 1;
        }
        if (!h->saw_syn) {
            h->saw_syn = 1;
            h->isn = p->seq;
        }
    }
    if (p->payload_len) {
        if (!h->have_base) {  // joined mid-connection: start at the first data seen
            h->have_base = 1;
            h->next_seq = p->seq;
        }
        segment(s, dir, p->seq, p->payload, p->payload_len);
    }
    if ((f & TCP_FIN) && h->have_base) {
        h->fin = 1;
        // the FIN takes up one sequence number, but only once everything before it has arrived
        if (p->seq + p->payload_len == h->next_seq) h->next_seq++;
    }
}

void streams_finish(StreamTable *t) {
    for (size_t i = 0; i < t->n; ++i) {
        Stream *s = &t->s[i];
        for (int dir = 0; dir < 2; ++dir) {
            Half *h = &s->h[dir];
            while (h->ooo) {
                int32_t delta = (int32_t)(h->ooo->seq - h->next_seq);
                if (delta > 0) add_gap(s, dir, (uint32_t)delta);  // the capture never saw these bytes
                drain(s, dir);
            }
        }
    }
}

// ---- reporting -----------------------------------------------------------------------

static const char *state_name(const Stream *s) {
    if (s->rst) return "reset";
    if (s->h[0].fin && s->h[1].fin) return "closed";
    if (s->syn && s->h[1].pkts == 0) return "no reply";
    if (s->h[0].fin || s->h[1].fin) return "closing";
    if (s->syn && s->synack) return "open";
    return "midstream";
}

static void endpoint(const Addr *a, uint16_t port, char *out, size_t n) {
    char t[64];
    addr_to_string(a, t, sizeof t);
    snprintf(out, n, "%s.%u", t, port);
}

void streams_list(const StreamTable *t, FILE *out) {
    if (!t->n) {
        fprintf(out, "no TCP connections\n");
        return;
    }
    fprintf(out, "TCP connections (%zu):\n", t->n);
    fprintf(out, "  %3s  %-24s  %-24s  %-9s  %9s  %9s  %s\n", "id", "client", "server", "state", "client->", "->client", "notes");
    for (size_t i = 0; i < t->n; ++i) {
        const Stream *s = &t->s[i];
        char c[96], v[96], notes[200] = "";
        endpoint(&s->client, s->cport, c, sizeof c);
        endpoint(&s->server, s->sport, v, sizeof v);
        uint64_t retrans = s->h[0].retrans + s->h[1].retrans, ooo = s->h[0].ooo_pkts + s->h[1].ooo_pkts;
        uint64_t miss = 0;
        int gaps = s->h[0].ngaps + s->h[1].ngaps;
        for (int d = 0; d < 2; ++d)
            for (int g = 0; g < s->h[d].ngaps; ++g) miss += s->h[d].gaps[g].missing;
        size_t len = 0;
        if (retrans) len += (size_t)snprintf(notes + len, sizeof notes - len, "%s%llu retransmitted", len ? ", " : "", (unsigned long long)retrans);
        if (ooo) len += (size_t)snprintf(notes + len, sizeof notes - len, "%s%llu out of order", len ? ", " : "", (unsigned long long)ooo);
        if (gaps) len += (size_t)snprintf(notes + len, sizeof notes - len, "%s%d gap%s, %llu bytes missing", len ? ", " : "", gaps, gaps == 1 ? "" : "s", (unsigned long long)miss);
        if (s->h[0].skipped || s->h[1].skipped) len += (size_t)snprintf(notes + len, sizeof notes - len, "%sdata past the %u MB limit not kept", len ? ", " : "", MAX_STREAM_BYTES >> 20);
        fprintf(out, "  %3zu  %-24s  %-24s  %-9s  %9llu  %9llu  %s\n", i, c, v, state_name(s),
                (unsigned long long)(s->h[0].len + s->h[0].skipped), (unsigned long long)(s->h[1].len + s->h[1].skipped), notes);
    }
}

static void put_text(const uint8_t *p, size_t n, FILE *out) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = p[i];
        if (c == '\r' && i + 1 < n && p[i + 1] == '\n') continue;
        if (c == '\n' || c == '\t' || (c >= 32 && c < 127)) fputc(c, out);
        else fprintf(out, "\\x%02x", c);
    }
}

static void put_hex(const uint8_t *p, size_t n, FILE *out) {
    for (size_t off = 0; off < n; off += 16) {
        fprintf(out, "  %06zx  ", off);
        for (size_t i = 0; i < 16; ++i) {
            if (off + i < n) fprintf(out, "%02x ", p[off + i]);
            else fputs("   ", out);
        }
        fputc(' ', out);
        for (size_t i = 0; i < 16 && off + i < n; ++i) fputc(p[off + i] >= 32 && p[off + i] < 127 ? p[off + i] : '.', out);
        fputc('\n', out);
    }
}

int streams_follow(const StreamTable *t, size_t id, FollowMode mode, FILE *out) {
    if (id >= t->n) return -1;
    const Stream *s = &t->s[id];
    if (mode == FOLLOW_RAW_CLIENT || mode == FOLLOW_RAW_SERVER) {
        const Half *h = &s->h[mode == FOLLOW_RAW_SERVER];
        fwrite(h->buf, 1, h->len, out);
        return 0;
    }
    char c[96], v[96];
    endpoint(&s->client, s->cport, c, sizeof c);
    endpoint(&s->server, s->sport, v, sizeof v);
    fprintf(out, "Stream %zu: %s <-> %s (%s)\n", id, c, v, state_name(s));
    int last = -1;
    for (size_t i = 0; i < s->nchunks; ++i) {
        const Chunk *ch = &s->chunks[i];
        if (ch->is_gap) {
            fprintf(out, "[%llu bytes missing from the capture]\n", (unsigned long long)ch->len);
            last = -1;
            continue;
        }
        if (ch->dir != last) {
            // length of the run of data going this way before the direction changes
            uint64_t run = 0;
            for (size_t k = i; k < s->nchunks && s->chunks[k].dir == ch->dir && !s->chunks[k].is_gap; ++k) run += s->chunks[k].len;
            fprintf(out, "\n--- %s -> %s (%llu bytes) ---\n", ch->dir == 0 ? "client" : "server", ch->dir == 0 ? "server" : "client",
                    (unsigned long long)run);
            last = ch->dir;
        }
        const uint8_t *data = s->h[ch->dir].buf + ch->off;
        if (mode == FOLLOW_HEX) put_hex(data, (size_t)ch->len, out);
        else put_text(data, (size_t)ch->len, out);
    }
    if (mode == FOLLOW_TEXT) fputc('\n', out);
    return 0;
}

const uint8_t *stream_data(const StreamTable *t, size_t id, int dir, size_t *len) {
    if (id >= t->n) { *len = 0; return NULL; }
    *len = t->s[id].h[dir].len;
    return t->s[id].h[dir].buf;
}

uint64_t stream_missing(const StreamTable *t, size_t id, int dir) {
    uint64_t m = 0;
    if (id >= t->n) return 0;
    for (int g = 0; g < t->s[id].h[dir].ngaps; ++g) m += t->s[id].h[dir].gaps[g].missing;
    return m;
}

// ---- HTTP ------------------------------------------------------------------------------

typedef struct {
    char line[160];     // the request line or status line
    char ctype[64];
    char host[96];
    int status;         // responses
    long clen;          // Content-Length, or -1
    int chunked;
    size_t body_len;    // decoded
    size_t end;         // offset just past this message
    int complete;
} HttpMsg;

static int ieq_prefix(const char *line, size_t n, const char *name) {
    size_t l = strlen(name);
    if (n < l + 1 || line[l] != ':') return 0;
    for (size_t i = 0; i < l; ++i)
        if (tolower((unsigned char)line[i]) != name[i]) return 0;
    return 1;
}

static const char *value_of(const char *line, size_t n, size_t skip) {
    while (skip < n && (line[skip] == ' ' || line[skip] == '\t')) skip++;
    return line + skip;
}

static void copy_text(char *dst, size_t cap, const char *src, size_t n) {
    size_t i = 0;
    while (i < n && i + 1 < cap) {
        dst[i] = (src[i] >= 32 && src[i] < 127) ? src[i] : '?';
        i++;
    }
    dst[i] = '\0';
}

static const uint8_t *find_sub(const uint8_t *h, size_t hn, const char *needle) {
    size_t nn = strlen(needle);
    if (hn < nn) return NULL;
    for (size_t i = 0; i + nn <= hn; ++i)
        if (h[i] == (uint8_t)needle[0] && !memcmp(h + i, needle, nn)) return h + i;
    return NULL;
}

// Read one HTTP message that starts at buf[pos]. Returns 1 if there is one.
static int http_message(const uint8_t *buf, size_t len, size_t pos, int is_response, int request_was_head, HttpMsg *m) {
    memset(m, 0, sizeof *m);
    m->clen = -1;
    const uint8_t *start = buf + pos;
    size_t avail = len - pos;
    if (is_response) {
        if (avail < 12 || memcmp(start, "HTTP/1.", 7)) return 0;
    } else {
        static const char *const methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "OPTIONS ", "PATCH ", "CONNECT ", NULL};
        int ok = 0;
        for (int i = 0; methods[i]; ++i)
            if (avail >= strlen(methods[i]) && !memcmp(start, methods[i], strlen(methods[i]))) ok = 1;
        if (!ok) return 0;
    }
    const uint8_t *hdr_end = find_sub(start, avail, "\r\n\r\n");
    size_t hdr_len;
    if (hdr_end) {
        hdr_len = (size_t)(hdr_end - start) + 4;
    } else {
        // the headers never finished (cut short): show what there is
        hdr_len = avail;
    }
    // header lines
    size_t i = 0;
    int first = 1;
    while (i < hdr_len) {
        const uint8_t *nl = memchr(start + i, '\n', hdr_len - i);
        size_t ll = nl ? (size_t)(nl - (start + i)) : hdr_len - i;
        size_t next = i + ll + (nl ? 1 : 0);
        if (ll && start[i + ll - 1] == '\r') ll--;
        const char *line = (const char *)start + i;
        if (first) {
            copy_text(m->line, sizeof m->line, line, ll);
            first = 0;
        } else if (ieq_prefix(line, ll, "content-length")) {
            m->clen = strtol(value_of(line, ll, 15), NULL, 10);
        } else if (ieq_prefix(line, ll, "transfer-encoding")) {
            const char *v = value_of(line, ll, 18);
            if (ll >= 18 && strstr(v, "chunked")) m->chunked = 1;
        } else if (ieq_prefix(line, ll, "content-type")) {
            const char *v = value_of(line, ll, 13);
            size_t vl = (size_t)(line + ll - v);
            const char *semi = memchr(v, ';', vl);
            copy_text(m->ctype, sizeof m->ctype, v, semi ? (size_t)(semi - v) : vl);
        } else if (ieq_prefix(line, ll, "host")) {
            copy_text(m->host, sizeof m->host, value_of(line, ll, 5), (size_t)(line + ll - value_of(line, ll, 5)));
        }
        i = next;
    }
    if (is_response && strlen(m->line) > 9) m->status = atoi(m->line + 9);
    if (!hdr_end) {
        m->end = len;
        m->complete = 0;
        return 1;
    }
    size_t body = pos + hdr_len;
    int no_body = !is_response ? (m->clen <= 0 && !m->chunked)
                               : (request_was_head || (m->status >= 100 && m->status < 200) || m->status == 204 || m->status == 304);
    if (no_body) {
        m->end = body;
        m->complete = 1;
    } else if (m->chunked) {
        size_t p = body, total = 0;
        m->complete = 0;
        m->end = len;
        for (;;) {
            const uint8_t *nl = p < len ? memchr(buf + p, '\n', len - p) : NULL;
            if (!nl) break;
            size_t sz = (size_t)strtoul((const char *)buf + p, NULL, 16);
            p = (size_t)(nl - buf) + 1;
            if (sz == 0) {  // the last chunk: optional trailer lines, then a blank line
                if (p + 2 <= len && buf[p] == '\r' && buf[p + 1] == '\n') {
                    m->end = p + 2;
                    m->complete = 1;
                } else {
                    const uint8_t *e = p < len ? find_sub(buf + p, len - p, "\r\n\r\n") : NULL;
                    m->end = e ? (size_t)(e - buf) + 4 : len;
                    m->complete = e != NULL;
                }
                break;
            }
            if (p + sz + 2 > len) { total += len - p; break; }
            total += sz;
            p += sz + 2;
        }
        m->body_len = total;
    } else if (m->clen >= 0) {
        if (body + (size_t)m->clen <= len) {
            m->end = body + (size_t)m->clen;
            m->complete = 1;
        } else {
            m->end = len;
        }
        m->body_len = m->end - body;
    } else {  // a response with no length: it runs until the connection closes
        m->end = len;
        m->body_len = len - body;
        m->complete = 1;
    }
    return 1;
}

void streams_http(const StreamTable *t, FILE *out) {
    size_t shown = 0, in_streams = 0;
    for (size_t id = 0; id < t->n; ++id) {
        const Stream *s = &t->s[id];
        HttpMsg reqs[256], resps[256];
        int nreq = 0, nresp = 0;
        size_t pos = 0;
        while (nreq < 256 && pos < s->h[0].len && http_message(s->h[0].buf, s->h[0].len, pos, 0, 0, &reqs[nreq])) {
            pos = reqs[nreq].end;
            nreq++;
            if (!reqs[nreq - 1].complete) break;
        }
        if (nreq == 0) continue;
        pos = 0;
        while (nresp < 256 && pos < s->h[1].len) {
            int head = nresp < nreq && !strncmp(reqs[nresp].line, "HEAD ", 5);
            if (!http_message(s->h[1].buf, s->h[1].len, pos, 1, head, &resps[nresp])) break;
            pos = resps[nresp].end;
            nresp++;
            if (!resps[nresp - 1].complete) break;
        }
        if (!in_streams++ && !shown) fprintf(out, "HTTP requests and responses:\n");
        for (int i = 0; i < nreq; ++i) {
            char method_target[160];
            const char *sp = strstr(reqs[i].line, " HTTP/");
            snprintf(method_target, sizeof method_target, "%.*s", sp ? (int)(sp - reqs[i].line) : (int)strlen(reqs[i].line), reqs[i].line);
            fprintf(out, "  stream %zu  %s", id, method_target);
            if (reqs[i].host[0]) fprintf(out, "  [%s]", reqs[i].host);
            if (reqs[i].body_len) fprintf(out, "  (sent %zu bytes)", reqs[i].body_len);
            if (i < nresp) {
                const char *status = strlen(resps[i].line) > 9 ? resps[i].line + 9 : resps[i].line;
                fprintf(out, "  ->  %s", status);
                if (resps[i].ctype[0]) fprintf(out, "  %s", resps[i].ctype);
                fprintf(out, "  %zu bytes%s", resps[i].body_len, resps[i].complete ? "" : " (incomplete)");
            } else {
                fprintf(out, "  ->  no response");
            }
            fputc('\n', out);
            shown++;
        }
    }
    if (!shown) fprintf(out, "no HTTP found in the TCP streams\n");
}
