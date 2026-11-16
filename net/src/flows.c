#include "flows.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t proto;
    Addr a, b;  // a is the numerically smaller endpoint, so both directions share one key
    uint16_t pa, pb;
    uint64_t pkts[2], bytes[2];  // [0]: a -> b, [1]: b -> a
    uint8_t flags[2];            // TCP flags seen in each direction
    int64_t first_s, last_s;
    uint32_t first_ns, last_ns;
    int next_in_bucket;
} Flow;

typedef struct {
    char text[160];
    unsigned count;
} Counted;

struct Stats {
    Flow *flows;
    size_t nflows, cap;
    int *buckets;      // hash table over the flows: index of the first flow in each bucket, or -1
    size_t nbuckets;
    uint64_t packets, bytes, non_ip, undecoded, truncated, bad_csum;
    uint64_t by_proto[6];  // TCP, UDP, ICMP, ICMPv6, other IP, non-IP
    int64_t first_s, last_s;
    uint32_t first_ns, last_ns;
    int have_time;
    Counted *dns;
    size_t ndns;
    Counted *http;
    size_t nhttp;
};

Stats *stats_new(void) { return calloc(1, sizeof(Stats)); }

void stats_free(Stats *s) {
    if (!s) return;
    free(s->flows);
    free(s->buckets);
    free(s->dns);
    free(s->http);
    free(s);
}

static int cmp_endpoint(const Addr *x, uint16_t px, const Addr *y, uint16_t py) {
    if (x->v6 != y->v6) return x->v6 - y->v6;
    int c = memcmp(x->b, y->b, 16);
    if (c) return c;
    return (int)px - (int)py;
}

static uint32_t flow_hash(uint8_t proto, const Addr *a, uint16_t pa, const Addr *b, uint16_t pb) {
    uint32_t h = 2166136261u ^ proto;
    for (int i = 0; i < 16; ++i) h = (h ^ a->b[i]) * 16777619u;
    for (int i = 0; i < 16; ++i) h = (h ^ b->b[i]) * 16777619u;
    h = (h ^ pa) * 16777619u;
    h = (h ^ pb) * 16777619u;
    return h ^ (h >> 15);
}

static void rehash(Stats *s, size_t nbuckets) {
    free(s->buckets);
    s->nbuckets = nbuckets;
    s->buckets = malloc(nbuckets * sizeof *s->buckets);
    for (size_t i = 0; i < nbuckets; ++i) s->buckets[i] = -1;
    for (size_t i = 0; i < s->nflows; ++i) {
        Flow *f = &s->flows[i];
        size_t b = flow_hash(f->proto, &f->a, f->pa, &f->b, f->pb) % nbuckets;
        f->next_in_bucket = s->buckets[b];
        s->buckets[b] = (int)i;
    }
}

static Flow *find_flow(Stats *s, const Packet *p, int *dir) {
    int swap = cmp_endpoint(&p->src, p->sport, &p->dst, p->dport) > 0;
    const Addr *a = swap ? &p->dst : &p->src, *b = swap ? &p->src : &p->dst;
    uint16_t pa = swap ? p->dport : p->sport, pb = swap ? p->sport : p->dport;
    *dir = swap;
    if (!s->nbuckets) rehash(s, 1024);
    size_t bucket = flow_hash(p->ip_proto, a, pa, b, pb) % s->nbuckets;
    for (int i = s->buckets[bucket]; i >= 0; i = s->flows[i].next_in_bucket) {
        Flow *f = &s->flows[i];
        if (f->proto == p->ip_proto && f->pa == pa && f->pb == pb && f->a.v6 == a->v6 && !memcmp(f->a.b, a->b, 16) &&
            !memcmp(f->b.b, b->b, 16))
            return f;
    }
    if (s->nflows == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->flows = realloc(s->flows, s->cap * sizeof *s->flows);
    }
    Flow *f = &s->flows[s->nflows++];
    memset(f, 0, sizeof *f);
    f->proto = p->ip_proto;
    f->a = *a;
    f->b = *b;
    f->pa = pa;
    f->pb = pb;
    f->first_s = p->ts_sec;
    f->first_ns = p->ts_nsec;
    f->next_in_bucket = s->buckets[bucket];
    s->buckets[bucket] = (int)(s->nflows - 1);
    if (s->nflows > s->nbuckets * 2) rehash(s, s->nbuckets * 4);
    return f;
}

static void count(Counted **arr, size_t *n, const char *text) {
    for (size_t i = 0; i < *n; ++i) {
        if (!strcmp((*arr)[i].text, text)) {
            (*arr)[i].count++;
            return;
        }
    }
    if (*n >= 2000) return;  // enough: this is a summary, not an index
    *arr = realloc(*arr, (*n + 1) * sizeof **arr);
    snprintf((*arr)[*n].text, sizeof (*arr)[*n].text, "%s", text);
    (*arr)[*n].count = 1;
    (*n)++;
}

void stats_add(Stats *s, const Packet *p) {
    if (p->reasm_frags) {  // already counted piece by piece: add only what the whole datagram says
        if (p->dns.present && p->dns.qdcount && !p->dns.response) {
            char key[300];
            snprintf(key, sizeof key, "%s %s", dns_type_name(p->dns.qtype) ? dns_type_name(p->dns.qtype) : "?", p->dns.qname);
            count(&s->dns, &s->ndns, key);
        }
        if (p->http == 1) count(&s->http, &s->nhttp, p->http_line);
        return;
    }
    s->packets++;
    s->bytes += p->origlen;
    if (!s->have_time) {
        s->first_s = p->ts_sec;
        s->first_ns = p->ts_nsec;
        s->have_time = 1;
    }
    s->last_s = p->ts_sec;
    s->last_ns = p->ts_nsec;
    if (p->truncated) s->truncated++;
    if (p->ip_csum == CSUM_BAD || p->l4_csum == CSUM_BAD) s->bad_csum++;

    if (!p->ip_version) {
        s->by_proto[5]++;
        s->non_ip++;
        return;
    }
    s->by_proto[p->l4 == 6 ? 0 : p->l4 == 17 ? 1 : p->l4 == 1 ? 2 : p->l4 == 58 ? 3 : 4]++;
    if (p->dns.present && p->dns.qdcount && !p->dns.response) {
        char key[300];
        snprintf(key, sizeof key, "%s %s", dns_type_name(p->dns.qtype) ? dns_type_name(p->dns.qtype) : "?", p->dns.qname);
        count(&s->dns, &s->ndns, key);
    }
    if (p->http == 1) count(&s->http, &s->nhttp, p->http_line);

    int dir;
    Flow *f = find_flow(s, p, &dir);
    f->pkts[dir]++;
    f->bytes[dir] += p->origlen;
    if (p->l4 == 6) f->flags[dir] |= p->tcp_flags;
    f->last_s = p->ts_sec;
    f->last_ns = p->ts_nsec;
}

static const char *tcp_state(const Flow *f) {
    uint8_t all = f->flags[0] | f->flags[1];
    if (all & TCP_RST) return "reset";
    if ((f->flags[0] & TCP_FIN) && (f->flags[1] & TCP_FIN)) return "closed";
    if ((all & TCP_SYN) && f->pkts[1] == 0) return "no reply";
    if (all & TCP_FIN) return "closing";
    if (all & TCP_SYN) return "open";
    return "midstream";
}

static int by_bytes(const void *x, const void *y) {
    const Flow *a = x, *b = y;
    uint64_t sa = a->bytes[0] + a->bytes[1], sb = b->bytes[0] + b->bytes[1];
    return sa < sb ? 1 : sa > sb ? -1 : 0;
}

static int by_count(const void *x, const void *y) {
    const Counted *a = x, *b = y;
    return a->count < b->count ? 1 : a->count > b->count ? -1 : strcmp(a->text, b->text);
}

static const char *pname(uint8_t proto) {
    switch (proto) {
        case 6: return "TCP";
        case 17: return "UDP";
        case 1: return "ICMP";
        case 58: return "ICMP6";
        default: return "IP";
    }
}

void stats_print(const Stats *cs, FILE *out, int top) {
    Stats *s = (Stats *)cs;  // sorted in place; the data is the same
    double span = s->have_time ? (double)(s->last_s - s->first_s) + ((double)s->last_ns - (double)s->first_ns) / 1e9 : 0;
    fprintf(out, "%llu packets, %llu bytes, %.3f seconds\n", (unsigned long long)s->packets, (unsigned long long)s->bytes, span);
    static const char *const names[] = {"TCP", "UDP", "ICMP", "ICMPv6", "other IP", "not IP (ARP, ...)"};
    for (int i = 0; i < 6; ++i)
        if (s->by_proto[i]) fprintf(out, "  %-18s %llu\n", names[i], (unsigned long long)s->by_proto[i]);
    if (s->truncated) fprintf(out, "  %llu packet(s) were cut short by the capture's snap length\n", (unsigned long long)s->truncated);
    if (s->bad_csum)
        fprintf(out, "  %llu packet(s) have a bad checksum (packets captured on the sending machine often do: the network card fills the checksum in afterwards)\n",
                (unsigned long long)s->bad_csum);

    if (s->nflows) {
        qsort(s->flows, s->nflows, sizeof *s->flows, by_bytes);
        fprintf(out, "\nBusiest conversations (%zu in total):\n", s->nflows);
        fprintf(out, "  %-5s %-45s %9s %9s  %s\n", "proto", "endpoints", "packets", "bytes", "state");
        for (size_t i = 0; i < s->nflows && (int)i < top; ++i) {
            const Flow *f = &s->flows[i];
            char a[64], b[64], ends[160];
            addr_to_string(&f->a, a, sizeof a);
            addr_to_string(&f->b, b, sizeof b);
            if (f->proto == 6 || f->proto == 17) snprintf(ends, sizeof ends, "%s.%u <-> %s.%u", a, f->pa, b, f->pb);
            else snprintf(ends, sizeof ends, "%s <-> %s", a, b);
            fprintf(out, "  %-5s %-45s %9llu %9llu  %s\n", pname(f->proto), ends,
                    (unsigned long long)(f->pkts[0] + f->pkts[1]), (unsigned long long)(f->bytes[0] + f->bytes[1]),
                    f->proto == 6 ? tcp_state(f) : "");
        }
    }
    if (s->ndns) {
        qsort(s->dns, s->ndns, sizeof *s->dns, by_count);
        fprintf(out, "\nDNS questions:\n");
        for (size_t i = 0; i < s->ndns && (int)i < top; ++i) fprintf(out, "  %4u  %s\n", s->dns[i].count, s->dns[i].text);
    }
    if (s->nhttp) {
        qsort(s->http, s->nhttp, sizeof *s->http, by_count);
        fprintf(out, "\nHTTP requests:\n");
        for (size_t i = 0; i < s->nhttp && (int)i < top; ++i) fprintf(out, "  %4u  %s\n", s->http[i].count, s->http[i].text);
    }
}
