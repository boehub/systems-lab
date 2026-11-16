#include "format.h"

#include <string.h>
#include <time.h>

static void mac(const uint8_t *m, char *out) {
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void ip4(const uint8_t *a, char *out) { snprintf(out, 16, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]); }

static void flags_text(uint8_t f, char *out) {
    char *o = out;
    if (f & TCP_FIN) *o++ = 'F';
    if (f & TCP_SYN) *o++ = 'S';
    if (f & TCP_RST) *o++ = 'R';
    if (f & TCP_PSH) *o++ = 'P';
    if (f & TCP_URG) *o++ = 'U';
    if (f & TCP_ACK) *o++ = '.';
    if (o == out) *o++ = '-';
    *o = '\0';
}

static const char *icmp_name(const Packet *p, char *buf, size_t n) {
    if (p->l4 == 1) {
        switch (p->icmp_type) {
            case 0: return "echo reply";
            case 8: return "echo request";
            case 3: return "destination unreachable";
            case 11: return "time exceeded";
            case 5: return "redirect";
        }
    } else {
        switch (p->icmp_type) {
            case 128: return "echo request";
            case 129: return "echo reply";
            case 1: return "destination unreachable";
            case 3: return "time exceeded";
            case 135: return "neighbor solicitation";
            case 136: return "neighbor advertisement";
            case 133: return "router solicitation";
            case 134: return "router advertisement";
        }
    }
    snprintf(buf, n, "type %u code %u", p->icmp_type, p->icmp_code);
    return buf;
}

static const char *qtype_text(uint16_t t, char *buf, size_t n) {
    const char *s = dns_type_name(t);
    if (s) return s;
    snprintf(buf, n, "TYPE%u", t);
    return buf;
}

void format_line(const Packet *p, int epoch, int show_csum, char *out, size_t n) {
    char when[40];
    if (epoch) {
        snprintf(when, sizeof when, "%lld.%06u", (long long)p->ts_sec, p->ts_nsec / 1000);
    } else {
        time_t t = (time_t)p->ts_sec;
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(when, sizeof when, "%02d:%02d:%02d.%06u", tm.tm_hour, tm.tm_min, tm.tm_sec, p->ts_nsec / 1000);
    }
    size_t o = (size_t)snprintf(out, n, "%s ", when);
    if (o >= n) return;
    char *w = out + o;
    size_t left = n - o;
#define PUT(...) do { int k_ = snprintf(w, left, __VA_ARGS__); if (k_ < 0 || (size_t)k_ >= left) { w += left ? left - 1 : 0; left = left ? 1 : 0; } else { w += k_; left -= (size_t)k_; } } while (0)

    char a[64], b[64], tmp[48], tmp2[16];
    if (p->ip_version) {
        addr_to_string(&p->src, a, sizeof a);
        addr_to_string(&p->dst, b, sizeof b);
        const char *fam = p->ip_version == 4 ? "IP" : "IP6";
        if (p->fragment && p->frag_offset > 0) {
            PUT("%s %s > %s: fragment, offset %d, proto %u", fam, a, b, p->frag_offset, p->ip_proto);
        } else if (p->l4 == 6) {
            char fl[8];
            flags_text(p->tcp_flags, fl);
            PUT("%s %s.%u > %s.%u: TCP [%s] seq %u", fam, a, p->sport, b, p->dport, fl, p->seq);
            if (p->tcp_flags & TCP_ACK) PUT(" ack %u", p->ack);
            PUT(" win %u len %u", p->window, p->payload_len);
            if (p->http) PUT(" | HTTP %s", p->http_line);
            else if (p->dns.present) PUT(" | DNS");
        } else if (p->l4 == 17) {
            PUT("%s %s.%u > %s.%u: UDP len %u", fam, a, p->sport, b, p->dport, p->payload_len);
            if (p->dns.present) {
                const DnsInfo *d = &p->dns;
                PUT(" | DNS %s id %u %s %s", d->response ? "response" : "query", d->id,
                    qtype_text(d->qtype, tmp2, sizeof tmp2), d->qdcount ? d->qname : "(no question)");
                if (d->response) {
                    PUT(" rcode %d", d->rcode);
                    for (int i = 0; i < d->nanswers; ++i) PUT("%s %s", i ? "," : " =>", d->answers[i]);
                    if (d->ancount > (unsigned)d->nanswers) PUT(" (+%u more)", d->ancount - (unsigned)d->nanswers);
                }
                if (d->malformed) PUT(" [malformed DNS]");
            }
        } else if (p->l4 == 1 || p->l4 == 58) {
            PUT("%s %s > %s: %s %s", fam, a, b, p->l4 == 1 ? "ICMP" : "ICMP6", icmp_name(p, tmp, sizeof tmp));
            int echo = p->l4 == 1 ? (p->icmp_type == 0 || p->icmp_type == 8) : (p->icmp_type == 128 || p->icmp_type == 129);
            if (echo) PUT(" id %u seq %u", p->icmp_id, p->icmp_seq);
        } else {
            PUT("%s %s > %s: proto %u, length %u", fam, a, b, p->ip_proto, p->ip_total);
        }
    } else if (p->has_arp) {
        char s[16], t[16], m[18];
        ip4(p->arp_spa, s);
        ip4(p->arp_tpa, t);
        mac(p->arp_sha, m);
        if (p->arp_op == 1) PUT("ARP who-has %s tell %s", t, s);
        else if (p->arp_op == 2) PUT("ARP %s is-at %s", s, m);
        else PUT("ARP op %u", p->arp_op);
    } else if (p->has_eth) {
        char s[18], d[18];
        mac(p->eth_src, s);
        mac(p->eth_dst, d);
        PUT("ETH %s > %s, type 0x%04x, length %u", s, d, p->ethertype, p->origlen);
    } else {
        PUT("(link layer not decoded)");
    }
    if (p->vlan >= 0) PUT(" [vlan %d]", p->vlan);
    if (show_csum && p->ip_csum == CSUM_BAD) PUT(" [bad IP checksum]");
    if (show_csum && p->l4_csum == CSUM_BAD) PUT(" [bad %s checksum]", p->l4 == 6 ? "TCP" : p->l4 == 17 ? "UDP" : "ICMP");
    if (p->truncated) PUT(" [truncated]");
    if (p->note) PUT(" [%s]", p->note);
#undef PUT
}

// ---- JSON -------------------------------------------------------------------------

static void jstr(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 32 || c >= 127) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static const char *csum_text(int c) { return c == CSUM_OK ? "ok" : c == CSUM_BAD ? "bad" : "na"; }

void format_json(const Packet *p, FILE *f) {
    char s[64], buf[64];
    fprintf(f, "{\"ts\":%lld.%09u,\"len\":%u,\"caplen\":%u", (long long)p->ts_sec, p->ts_nsec, p->origlen, p->caplen);
    if (p->has_eth) {
        mac(p->eth_src, s);
        fprintf(f, ",\"eth_src\":\"%s\"", s);
        mac(p->eth_dst, s);
        fprintf(f, ",\"eth_dst\":\"%s\",\"ethertype\":%u", s, p->ethertype);
        if (p->vlan >= 0) fprintf(f, ",\"vlan\":%d", p->vlan);
    }
    if (p->has_arp) {
        fprintf(f, ",\"arp\":{\"op\":%u", p->arp_op);
        mac(p->arp_sha, s);
        fprintf(f, ",\"sha\":\"%s\"", s);
        ip4(p->arp_spa, s);
        fprintf(f, ",\"spa\":\"%s\"", s);
        mac(p->arp_tha, s);
        fprintf(f, ",\"tha\":\"%s\"", s);
        ip4(p->arp_tpa, s);
        fprintf(f, ",\"tpa\":\"%s\"}", s);
    }
    if (p->ip_version) {
        addr_to_string(&p->src, s, sizeof s);
        addr_to_string(&p->dst, buf, sizeof buf);
        fprintf(f, ",\"ip\":%d,\"src\":\"%s\",\"dst\":\"%s\",\"proto\":%u,\"ttl\":%u,\"ip_csum\":\"%s\"", p->ip_version, s, buf,
                p->ip_proto, p->ttl, csum_text(p->ip_csum));
        if (p->fragment) fprintf(f, ",\"frag_offset\":%d", p->frag_offset);
    }
    if (p->l4 == 6 || p->l4 == 17) {
        fprintf(f, ",\"sport\":%u,\"dport\":%u,\"payload\":%u,\"l4_csum\":\"%s\"", p->sport, p->dport, p->payload_len,
                csum_text(p->l4_csum));
        if (p->l4 == 6) fprintf(f, ",\"seq\":%u,\"ack\":%u,\"flags\":%u,\"win\":%u", p->seq, p->ack, p->tcp_flags, p->window);
    }
    if (p->l4 == 1 || p->l4 == 58)
        fprintf(f, ",\"icmp_type\":%u,\"icmp_code\":%u,\"icmp_id\":%u,\"icmp_seq\":%u,\"l4_csum\":\"%s\"", p->icmp_type,
                p->icmp_code, p->icmp_id, p->icmp_seq, csum_text(p->l4_csum));
    if (p->dns.present) {
        const DnsInfo *d = &p->dns;
        fprintf(f, ",\"dns\":{\"id\":%u,\"qr\":%d,\"opcode\":%d,\"rcode\":%d,\"qd\":%u,\"an\":%u,\"ns\":%u,\"ar\":%u", d->id,
                d->response, d->opcode, d->rcode, d->qdcount, d->ancount, d->nscount, d->arcount);
        if (d->qdcount) {
            fputs(",\"qname\":", f);
            jstr(f, d->qname);
            fprintf(f, ",\"qtype\":%u", d->qtype);
        }
        fputs(",\"answers\":[", f);
        for (int i = 0; i < d->nanswers; ++i) {
            if (i) fputc(',', f);
            jstr(f, d->answers[i]);
        }
        fprintf(f, "],\"malformed\":%s}", d->malformed ? "true" : "false");
    }
    if (p->http) {
        fprintf(f, ",\"http\":{\"kind\":\"%s\",\"line\":", p->http == 1 ? "request" : "response");
        jstr(f, p->http_line);
        fputc('}', f);
    }
    if (p->truncated) fputs(",\"truncated\":true", f);
    if (p->note) {
        fputs(",\"note\":", f);
        jstr(f, p->note);
    }
    fputc('}', f);
}

void format_hex(const uint8_t *data, size_t len, size_t max, FILE *out) {
    if (len > max) len = max;
    for (size_t i = 0; i < len; i += 16) {
        fprintf(out, "    %04zx  ", i);
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < len) fprintf(out, "%02x ", data[i + j]);
            else fputs("   ", out);
            if (j == 7) fputc(' ', out);
        }
        fputc(' ', out);
        for (size_t j = 0; j < 16 && i + j < len; ++j) fputc(data[i + j] >= 32 && data[i + j] < 127 ? data[i + j] : '.', out);
        fputc('\n', out);
    }
}
