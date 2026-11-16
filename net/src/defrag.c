#include "defrag.h"

#include <stdlib.h>
#include <string.h>

#define MAX_DATAGRAMS 256   // incomplete datagrams kept at once; the oldest is dropped beyond this
#define MAX_PIECES 512      // fragments per datagram
#define MAX_TOTAL 65535u    // the largest datagram payload we rebuild

typedef struct {
    uint32_t off, len;
    uint8_t *data;
} Piece;

typedef struct {
    int used;
    Addr src, dst;
    uint8_t proto_key;       // the IP protocol field (IPv4) or 44 (IPv6), part of the key
    uint32_t id;
    Piece *pieces;
    int npieces, cap;
    int have_last;
    uint32_t total;          // payload length, known once the last piece has arrived
    uint8_t hdr[60];         // the header of the piece at offset 0
    uint8_t hdr_len, have_first;
    uint8_t first_proto;     // what the reassembled data carries
    uint64_t age;
} Dgram;

struct Defrag {
    Dgram slots[MAX_DATAGRAMS];
    uint64_t clock;
    size_t dropped;
};

Defrag *defrag_new(void) { return calloc(1, sizeof(Defrag)); }

static void clear(Dgram *g) {
    for (int i = 0; i < g->npieces; ++i) free(g->pieces[i].data);
    free(g->pieces);
    memset(g, 0, sizeof *g);
}

void defrag_free(Defrag *d) {
    if (!d) return;
    for (int i = 0; i < MAX_DATAGRAMS; ++i) clear(&d->slots[i]);
    free(d);
}

size_t defrag_pending(const Defrag *d) {
    size_t n = 0;
    for (int i = 0; i < MAX_DATAGRAMS; ++i) n += d->slots[i].used;
    return n;
}

size_t defrag_dropped(const Defrag *d) { return d->dropped; }

static Dgram *find_slot(Defrag *d, const Packet *p) {
    uint8_t key = p->ip_version == 6 ? 44 : p->frag_proto;
    Dgram *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_DATAGRAMS; ++i) {
        Dgram *g = &d->slots[i];
        if (!g->used) {
            if (!free_slot) free_slot = g;
            continue;
        }
        if (g->id == p->frag_id && g->proto_key == key && g->src.v6 == p->src.v6 && !memcmp(g->src.b, p->src.b, 16) &&
            !memcmp(g->dst.b, p->dst.b, 16))
            return g;
        if (!oldest || g->age < oldest->age) oldest = g;
    }
    Dgram *g = free_slot;
    if (!g) {  // table full: give up on the oldest datagram
        g = oldest;
        clear(g);
        d->dropped++;
    }
    g->used = 1;
    g->src = p->src;
    g->dst = p->dst;
    g->proto_key = key;
    g->id = p->frag_id;
    return g;
}

// Try to assemble; returns 1 and fills the outputs when every byte has arrived.
static int try_complete(Dgram *g, uint8_t **out, size_t *len, int *nfrags) {
    if (!g->have_last || !g->have_first) return 0;
    uint8_t *data = calloc(g->total ? g->total : 1, 1);
    uint8_t *have = calloc(g->total ? g->total : 1, 1);
    size_t covered = 0;
    for (int i = 0; i < g->npieces; ++i) {  // earlier pieces win where they overlap
        const Piece *pc = &g->pieces[i];
        for (uint32_t k = 0; k < pc->len && pc->off + k < g->total; ++k) {
            if (!have[pc->off + k]) {
                have[pc->off + k] = 1;
                data[pc->off + k] = pc->data[k];
                covered++;
            }
        }
    }
    free(have);
    if (covered != g->total) {
        free(data);
        return 0;
    }
    int v6 = g->src.v6;
    size_t hl = v6 ? 40 : 20;
    uint8_t *pkt = malloc(hl + g->total);
    memcpy(pkt, g->hdr, hl);
    if (v6) {
        pkt[4] = (uint8_t)(g->total >> 8);
        pkt[5] = (uint8_t)g->total;
        pkt[6] = g->first_proto;
    } else {
        pkt[0] = 0x45;  // header options are dropped
        pkt[2] = (uint8_t)((hl + g->total) >> 8);
        pkt[3] = (uint8_t)(hl + g->total);
        pkt[6] = pkt[7] = 0;  // no longer a fragment
        pkt[10] = pkt[11] = 0;
        uint16_t sum = csum_fold(csum_add(0, pkt, 20));
        pkt[10] = (uint8_t)(sum >> 8);
        pkt[11] = (uint8_t)sum;
    }
    memcpy(pkt + hl, data, g->total);
    free(data);
    *out = pkt;
    *len = hl + g->total;
    *nfrags = g->npieces;
    return 1;
}

int defrag_add(Defrag *d, const Packet *p, uint8_t **out, size_t *len, int *nfrags) {
    if (!p->fragment || !p->frag_data || !p->ip_hdr || p->frag_len == 0) return 0;
    if (p->ip_version != 4 && p->ip_version != 6) return 0;
    if ((uint64_t)p->frag_offset + p->frag_len > MAX_TOTAL) return 0;
    Dgram *g = find_slot(d, p);
    g->age = ++d->clock;
    if (g->npieces >= MAX_PIECES) {
        clear(g);
        d->dropped++;
        return 0;
    }
    if (g->npieces == g->cap) {
        g->cap = g->cap ? g->cap * 2 : 8;
        g->pieces = realloc(g->pieces, (size_t)g->cap * sizeof *g->pieces);
    }
    Piece *pc = &g->pieces[g->npieces++];
    pc->off = (uint32_t)p->frag_offset;
    pc->len = p->frag_len;
    pc->data = malloc(p->frag_len);
    memcpy(pc->data, p->frag_data, p->frag_len);

    if (p->frag_offset == 0) {
        size_t hl = p->ip_version == 6 ? 40 : 20;
        if (p->ip_hdr_len >= hl && hl <= sizeof g->hdr) {
            memcpy(g->hdr, p->ip_hdr, hl);
            g->hdr_len = (uint8_t)hl;
            g->have_first = 1;
            g->first_proto = p->frag_proto;
        }
    }
    if (!p->frag_more) {
        uint32_t t = (uint32_t)p->frag_offset + p->frag_len;
        if (g->have_last && g->total != t) {  // two different ends: the pieces do not belong together
            clear(g);
            d->dropped++;
            return 0;
        }
        g->have_last = 1;
        g->total = t;
    }
    if (g->have_last && g->total > MAX_TOTAL) {
        clear(g);
        d->dropped++;
        return 0;
    }
    if (try_complete(g, out, len, nfrags)) {
        clear(g);
        return 1;
    }
    return 0;
}
