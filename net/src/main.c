// pktscope: read a packet capture (or sniff an interface) and say what is in it.

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "decode.h"
#include "filter.h"
#include "flows.h"
#include "format.h"
#include "live.h"
#include "pcap.h"

static const char *usage =
    "usage: pktscope [options] FILE.pcap   (FILE of - reads standard input)\n"
    "       pktscope [options] --live IFACE\n"
    "\n"
    "Reads a pcap or pcapng capture and prints one line per packet.\n"
    "\n"
    "options:\n"
    "  -f, --filter EXPR   only packets matching EXPR, e.g. \"tcp and port 80\"\n"
    "  -c, --count N       stop after N packets\n"
    "  -s, --summary       print totals, busiest conversations, DNS and HTTP seen\n"
    "      --json          one JSON object per packet (newline-delimited)\n"
    "  -x, --hex           also dump the payload bytes\n"
    "      --check         tag packets whose checksums don't verify (captures taken on the\n"
    "                      sending machine show many of these: the network card fills them in later)\n"
    "      --epoch         show times as seconds since 1970 (default: UTC time of day)\n"
    "      --top N         rows in the summary tables (default 10)\n"
    "      --live IFACE    capture from a network interface (Linux, needs root)\n"
    "  -w, --write FILE    also save the matching packets to a pcap file\n"
    "  -q, --quiet         with -w: do not print the packets\n"
    "  -h, --help          this help\n"
    "\n"
    "filter words: tcp udp icmp icmp6 ip ip6 arp dns http syn fin rst vlan badcsum\n"
    "              host A, net A/N, port N (each optionally with src or dst),\n"
    "              greater N, less N; combine with and / or / not and ( ).\n";

typedef struct {
    const char *file, *filter, *live, *write;
    long count;
    int summary, json, hex, epoch, check, quiet, top;
} Options;

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

static int parse_args(int argc, char **argv, Options *o) {
    memset(o, 0, sizeof *o);
    o->top = 10;
    o->count = -1;
    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        char *end;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            fputs(usage, stdout);
            exit(0);
        } else if (!strcmp(a, "-f") || !strcmp(a, "--filter")) {
            if (!v) goto need;
            o->filter = argv[++i];
        } else if (!strcmp(a, "-c") || !strcmp(a, "--count") || !strcmp(a, "--top")) {
            if (!v) goto need;
            long n = strtol(v, &end, 10);
            if (!*v || *end || n < 1) {
                fprintf(stderr, "pktscope: %s needs a positive whole number\n", a);
                return -1;
            }
            if (!strcmp(a, "--top")) o->top = (int)n; else o->count = n;
            i++;
        } else if (!strcmp(a, "-s") || !strcmp(a, "--summary")) {
            o->summary = 1;
        } else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) {
            o->quiet = 1;
        } else if (!strcmp(a, "--json")) {
            o->json = 1;
        } else if (!strcmp(a, "-x") || !strcmp(a, "--hex")) {
            o->hex = 1;
        } else if (!strcmp(a, "--epoch")) {
            o->epoch = 1;
        } else if (!strcmp(a, "--check")) {
            o->check = 1;
        } else if (!strcmp(a, "--live")) {
            if (!v) goto need;
            o->live = argv[++i];
        } else if (!strcmp(a, "-w") || !strcmp(a, "--write")) {
            if (!v) goto need;
            o->write = argv[++i];
        } else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "pktscope: unknown option %s\n", a);
            return -1;
        } else if (o->file) {
            fprintf(stderr, "pktscope: only one capture file can be given\n");
            return -1;
        } else {
            o->file = a;
        }
        continue;
    need:
        fprintf(stderr, "pktscope: %s needs a value\n", a);
        return -1;
    }
    if (!o->file && !o->live) {
        fputs(usage, stderr);
        return -1;
    }
    if (o->file && o->live) {
        fprintf(stderr, "pktscope: give a file or --live, not both\n");
        return -1;
    }
    return 0;
}

typedef struct {
    Options *o;
    Filter *filter;
    Stats *stats;
    long shown;
    FILE *w;          // -w output, or NULL
    int w_linktype;   // link type of the output file (-1 until the first packet)
    long w_skipped;   // packets not written because their link type differs
} Ctx;

// Handle one captured frame. Returns 1 to keep going, 0 once the count is reached.
static int handle(Ctx *c, const PcapRecord *rec) {
    Packet p;
    decode_packet(rec->data, rec->caplen, rec->linktype, &p);
    p.ts_sec = rec->ts_sec;
    p.ts_nsec = rec->ts_nsec;
    p.origlen = rec->origlen;
    if (!filter_match(c->filter, &p)) return 1;
    if (c->w) {
        if (c->w_linktype < 0) {
            c->w_linktype = rec->linktype;
            pcap_write_header(c->w, rec->linktype, 65535);
        }
        if (rec->linktype == c->w_linktype)
            pcap_write_record(c->w, rec->ts_sec, rec->ts_nsec / 1000, rec->data, rec->caplen, rec->origlen);
        else
            c->w_skipped++;
    }
    if (c->o->quiet && !c->o->summary) {
        /* nothing to print */
    } else if (c->o->summary) {
        stats_add(c->stats, &p);
    } else if (c->o->json) {
        format_json(&p, stdout);
        fputc('\n', stdout);
    } else {
        char line[1024];
        format_line(&p, c->o->epoch, c->o->check, line, sizeof line);
        puts(line);
        if (c->o->hex && p.payload_len) format_hex(p.payload, p.payload_len, 256, stdout);
    }
    c->shown++;
    return c->o->count < 0 || c->shown < c->o->count;
}

static int run_file(Ctx *c) {
    PcapReader r;
    char err[200];
    if (pcap_open(&r, c->o->file, err, sizeof err) != 0) {
        fprintf(stderr, "pktscope: %s\n", err);
        return 1;
    }
    PcapRecord rec;
    int status = 0, rc;
    while ((rc = pcap_next(&r, &rec, err, sizeof err)) == 1) {
        if (!handle(c, &rec)) break;
    }
    if (rc < 0) {
        fflush(stdout);
        fprintf(stderr, "pktscope: %s: %s\n", c->o->file, err);
        status = 1;
    }
    pcap_close(&r);
    return status;
}

static int run_live(Ctx *c) {
    char err[200];
    int linktype;
    int fd = live_open(c->o->live, &linktype, err, sizeof err);
    if (fd < 0) {
        fprintf(stderr, "pktscope: %s\n", err);
        return 1;
    }
    if (c->w) {  // a live capture knows its link type up front
        c->w_linktype = linktype;
        pcap_write_header(c->w, linktype, 65535);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    static uint8_t buf[65535];
    int status = 0;
    while (!stop) {
        uint32_t len, orig, nsec;
        int64_t sec;
        int r = live_read(fd, buf, sizeof buf, &len, &orig, &sec, &nsec, 200);
        if (r < 0) {
            fprintf(stderr, "pktscope: capture failed: %s\n", strerror(errno));
            status = 1;
            break;
        }
        if (r == 0) continue;
        PcapRecord rec = {sec, nsec, len, orig, linktype, buf};
        int more = handle(c, &rec);
        fflush(stdout);
        if (!more) break;
    }
    live_close(fd);
    return status;
}

int main(int argc, char **argv) {
    Options o;
    if (parse_args(argc, argv, &o) != 0) return 2;
    char err[200];
    Filter *f = filter_compile(o.filter, err, sizeof err);
    if (!f) {
        fprintf(stderr, "pktscope: filter: %s\n", err);
        return 2;
    }
    Ctx c = {&o, f, o.summary ? stats_new() : NULL, 0, NULL, -1, 0};
    if (o.write) {
        c.w = fopen(o.write, "wb");
        if (!c.w) {
            fprintf(stderr, "pktscope: cannot write %s: %s\n", o.write, strerror(errno));
            return 1;
        }
    }
    int status = o.live ? run_live(&c) : run_file(&c);
    if (o.summary) {
        fflush(stdout);
        stats_print(c.stats, stdout, o.top);
    }
    if (c.w) {
        if (c.w_linktype < 0) pcap_write_header(c.w, LT_ETHERNET, 65535);  // nothing matched: still a valid file
        if (fclose(c.w) != 0) {
            fprintf(stderr, "pktscope: error writing %s\n", o.write);
            status = status ? status : 1;
        }
        if (c.w_skipped)
            fprintf(stderr, "pktscope: %ld packet(s) not written: link type differs from the first packet's\n", c.w_skipped);
    }
    stats_free(c.stats);
    filter_free(f);
    if (fflush(stdout) != 0 && errno != EPIPE) status = status ? status : 1;
    return status;
}
