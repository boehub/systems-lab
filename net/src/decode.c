#include "decode.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---- checksums -----------------------------------------------------------------

uint32_t csum_add(uint32_t sum, const uint8_t *data, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += rd16(data + i);
    if (len & 1) sum += (uint32_t)data[len - 1] << 8;
    return sum;
}

uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// A segment whose stored checksum is right sums (with the pseudo-header) to zero.
static int verify_l4(const Packet *p, const uint8_t *seg, size_t seglen, uint8_t proto) {
    uint32_t sum = 0;
    if (p->src.v6) {
        sum = csum_add(sum, p->src.b, 16);
        sum = csum_add(sum, p->dst.b, 16);
        uint8_t tail[8] = {0, 0, (uint8_t)(seglen >> 8), (uint8_t)seglen, 0, 0, 0, proto};
        sum = csum_add(sum, tail, 8);
    } else {
        sum = csum_add(sum, p->src.b, 4);
        sum = csum_add(sum, p->dst.b, 4);
        uint8_t tail[4] = {0, proto, (uint8_t)(seglen >> 8), (uint8_t)seglen};
        sum = csum_add(sum, tail, 4);
    }
    sum = csum_add(sum, seg, seglen);
    return csum_fold(sum) == 0 ? CSUM_OK : CSUM_BAD;
}

// ---- addresses -----------------------------------------------------------------

void addr_to_string(const Addr *a, char *out, size_t n) {
    if (!inet_ntop(a->v6 ? AF_INET6 : AF_INET, a->b, out, (socklen_t)n)) snprintf(out, n, "?");
}

int addr_parse(const char *text, Addr *a) {
    memset(a, 0, sizeof *a);
    if (inet_pton(AF_INET, text, a->b) == 1) return 0;
    if (inet_pton(AF_INET6, text, a->b) == 1) {
        a->v6 = 1;
        return 0;
    }
    return -1;
}

// ---- DNS -----------------------------------------------------------------------

const char *dns_type_name(uint16_t t) {
    switch (t) {
        case 1: return "A";
        case 2: return "NS";
        case 5: return "CNAME";
        case 6: return "SOA";
        case 12: return "PTR";
        case 15: return "MX";
        case 16: return "TXT";
        case 28: return "AAAA";
        case 33: return "SRV";
        case 65: return "HTTPS";
        case 255: return "ANY";
        default: return NULL;
    }
}

int dns_read_name(const uint8_t *msg, size_t len, size_t off, char *out, size_t outsz, size_t *next) {
    size_t pos = off, o = 0;
    int jumps = 0;
    size_t after = 0;
    int jumped = 0;
    if (outsz == 0) return -1;
    for (;;) {
        if (pos >= len) return -1;
        uint8_t c = msg[pos];
        if (c == 0) {  // end of name
            if (!jumped) after = pos + 1;
            break;
        }
        if ((c & 0xC0) == 0xC0) {  // compression pointer: two bytes, 14-bit offset
            if (pos + 1 >= len) return -1;
            if (!jumped) after = pos + 2;
            jumped = 1;
            if (++jumps > 16) return -1;  // a loop, or an absurd chain
            pos = (size_t)(((c & 0x3F) << 8) | msg[pos + 1]);
            continue;
        }
        if (c & 0xC0) return -1;  // the 01 and 10 label types are not valid
        size_t l = c;
        if (pos + 1 + l > len) return -1;
        if (o + l + 2 > outsz) return -1;  // would not fit (also caps names at the buffer size)
        if (o > 0) out[o++] = '.';
        for (size_t i = 0; i < l; ++i) {
            uint8_t ch = msg[pos + 1 + i];
            out[o++] = (ch >= 32 && ch < 127) ? (char)ch : '?';
        }
        pos += 1 + l;
    }
    if (o == 0) out[o++] = '.';  // the root
    out[o] = '\0';
    if (next) *next = after;
    return 0;
}

static void dns_answer_text(const uint8_t *msg, size_t len, size_t rdoff, uint16_t type, uint16_t rdlen,
                            char *out, size_t outsz) {
    const char *tn = dns_type_name(type);
    char tname[16];
    if (!tn) {
        snprintf(tname, sizeof tname, "TYPE%u", type);
        tn = tname;
    }
    char data[80] = "";
    if (type == 1 && rdlen == 4) {
        snprintf(data, sizeof data, "%u.%u.%u.%u", msg[rdoff], msg[rdoff + 1], msg[rdoff + 2], msg[rdoff + 3]);
    } else if (type == 28 && rdlen == 16) {
        inet_ntop(AF_INET6, msg + rdoff, data, sizeof data);
    } else if (type == 2 || type == 5 || type == 12) {
        if (dns_read_name(msg, len, rdoff, data, sizeof data, NULL) != 0) snprintf(data, sizeof data, "(bad name)");
    } else if (type == 15 && rdlen > 2) {
        char host[64];
        if (dns_read_name(msg, len, rdoff + 2, host, sizeof host, NULL) == 0)
            snprintf(data, sizeof data, "%u %s", rd16(msg + rdoff), host);
    } else if (type == 16 && rdlen >= 1) {
        size_t l = msg[rdoff];
        if (l > rdlen - 1u) l = rdlen - 1u;
        if (l > 60) l = 60;
        snprintf(data, sizeof data, "\"%.*s\"", (int)l, (const char *)msg + rdoff + 1);
    }
    snprintf(out, outsz, "%s %s", tn, data);
}

static void decode_dns(const uint8_t *m, size_t len, DnsInfo *d) {
    memset(d, 0, sizeof *d);
    if (len < 12) return;
    d->present = 1;
    d->id = rd16(m);
    uint16_t flags = rd16(m + 2);
    d->response = (flags >> 15) & 1;
    d->opcode = (flags >> 11) & 0xF;
    d->rcode = flags & 0xF;
    d->qdcount = rd16(m + 4);
    d->ancount = rd16(m + 6);
    d->nscount = rd16(m + 8);
    d->arcount = rd16(m + 10);

    size_t pos = 12;
    if (d->qdcount >= 1) {
        size_t next;
        if (dns_read_name(m, len, pos, d->qname, sizeof d->qname, &next) != 0 || next + 4 > len) {
            d->malformed = 1;
            return;
        }
        d->qtype = rd16(m + next);
        pos = next + 4;
        // skip any further questions
        for (unsigned i = 1; i < d->qdcount; ++i) {
            char tmp[256];
            if (dns_read_name(m, len, pos, tmp, sizeof tmp, &next) != 0 || next + 4 > len) {
                d->malformed = 1;
                return;
            }
            pos = next + 4;
        }
    }
    for (unsigned i = 0; i < d->ancount && d->nanswers < DNS_MAX_ANSWERS; ++i) {
        char tmp[256];
        size_t next;
        if (dns_read_name(m, len, pos, tmp, sizeof tmp, &next) != 0 || next + 10 > len) {
            d->malformed = 1;
            return;
        }
        uint16_t type = rd16(m + next), rdlen = rd16(m + next + 8);
        size_t rdoff = next + 10;
        if (rdoff + rdlen > len) {
            d->malformed = 1;
            return;
        }
        dns_answer_text(m, len, rdoff, type, rdlen, d->answers[d->nanswers++], sizeof d->answers[0]);
        pos = rdoff + rdlen;
    }
}

// ---- HTTP ----------------------------------------------------------------------

static int starts_with(const uint8_t *p, size_t n, const char *s) {
    size_t l = strlen(s);
    return n >= l && memcmp(p, s, l) == 0;
}

static void detect_http(const uint8_t *pl, size_t n, Packet *p) {
    static const char *const methods[] = {"GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "OPTIONS ", "PATCH ",
                                          "CONNECT ", NULL};
    int kind = 0;
    if (starts_with(pl, n, "HTTP/1.")) {
        kind = 2;
    } else {
        for (int i = 0; methods[i]; ++i)
            if (starts_with(pl, n, methods[i])) kind = 1;
    }
    if (!kind) return;
    size_t i = 0;
    while (i < n && i < sizeof p->http_line - 1 && pl[i] != '\r' && pl[i] != '\n') {
        p->http_line[i] = (pl[i] >= 32 && pl[i] < 127) ? (char)pl[i] : '?';
        i++;
    }
    p->http_line[i] = '\0';
    // a request must name its protocol version; otherwise a stray "GET " in a binary payload
    // would be mistaken for HTTP
    if (kind == 1 && !strstr(p->http_line, " HTTP/")) {
        p->http_line[0] = '\0';
        return;
    }
    p->http = kind;
}

// ---- transport -----------------------------------------------------------------

// `seg` points at the transport header; `avail` bytes were captured; `seglen` is how many the
// IP header says there should be.
static void decode_l4(Packet *p, const uint8_t *seg, size_t avail, size_t seglen, int whole_datagram) {
    switch (p->ip_proto) {
        case 6: {  // TCP
            if (avail < 20) { p->note = "truncated TCP header"; p->truncated = 1; return; }
            size_t hl = (size_t)(seg[12] >> 4) * 4;
            if (hl < 20 || hl > seglen) { p->note = "bad TCP header length"; return; }
            if (hl > avail) { p->note = "truncated TCP header"; p->truncated = 1; return; }
            p->l4 = 6;
            p->sport = rd16(seg);
            p->dport = rd16(seg + 2);
            p->seq = rd32(seg + 4);
            p->ack = rd32(seg + 8);
            p->tcp_flags = seg[13];
            p->window = rd16(seg + 14);
            p->payload = seg + hl;
            p->payload_len = (uint32_t)(avail - hl);
            if (whole_datagram && avail == seglen) p->l4_csum = verify_l4(p, seg, seglen, 6);
            if (p->payload_len) detect_http(p->payload, p->payload_len, p);
            if (p->payload_len > 2 && (p->sport == 53 || p->dport == 53)) {
                size_t l = rd16(p->payload);  // DNS over TCP: a two-byte length comes first
                if (l + 2 <= p->payload_len) decode_dns(p->payload + 2, l, &p->dns);
            }
            break;
        }
        case 17: {  // UDP
            if (avail < 8) { p->note = "truncated UDP header"; p->truncated = 1; return; }
            size_t ulen = rd16(seg + 4);
            if (ulen < 8) { p->note = "bad UDP length"; return; }
            p->l4 = 17;
            p->sport = rd16(seg);
            p->dport = rd16(seg + 2);
            size_t have = avail < ulen ? avail : ulen;
            p->payload = seg + 8;
            p->payload_len = (uint32_t)(have - 8);
            uint16_t wire = rd16(seg + 6);
            if (whole_datagram && ulen <= avail && ulen <= seglen) {
                if (wire == 0 && !p->src.v6) p->l4_csum = CSUM_NA;  // IPv4 UDP may skip the checksum
                else p->l4_csum = verify_l4(p, seg, ulen, 17);
            }
            if (p->payload_len && (p->sport == 53 || p->dport == 53 || p->sport == 5353 || p->dport == 5353))
                decode_dns(p->payload, p->payload_len, &p->dns);
            break;
        }
        case 1:    // ICMP
        case 58: { // ICMPv6
            if (avail < 4) { p->note = "truncated ICMP header"; p->truncated = 1; return; }
            p->l4 = p->ip_proto;
            p->icmp_type = seg[0];
            p->icmp_code = seg[1];
            int echo = p->ip_proto == 1 ? (seg[0] == 0 || seg[0] == 8) : (seg[0] == 128 || seg[0] == 129);
            if (echo && avail >= 8) {
                p->icmp_id = rd16(seg + 4);
                p->icmp_seq = rd16(seg + 6);
            }
            p->payload = seg + (avail >= 8 ? 8 : avail);
            p->payload_len = (uint32_t)(avail >= 8 ? avail - 8 : 0);
            if (whole_datagram && avail == seglen) {
                if (p->ip_proto == 58) p->l4_csum = verify_l4(p, seg, seglen, 58);
                else p->l4_csum = csum_fold(csum_add(0, seg, seglen)) == 0 ? CSUM_OK : CSUM_BAD;
            }
            break;
        }
        default:
            break;
    }
}

static int decode_ipv4(const uint8_t *d, size_t n, Packet *p) {
    if (n < 20) { p->note = "truncated IPv4 header"; p->truncated = 1; return 0; }
    if ((d[0] >> 4) != 4) { p->note = "bad IP version"; return 0; }
    size_t ihl = (size_t)(d[0] & 0xF) * 4;
    uint16_t total = rd16(d + 2);
    if (ihl < 20 || total < ihl) { p->note = "bad IPv4 header length"; return 0; }
    if (ihl > n) { p->note = "truncated IPv4 header"; p->truncated = 1; return 0; }
    p->ip_version = 4;
    p->ip_total = total;
    p->ttl = d[8];
    p->ip_proto = d[9];
    p->src.v6 = p->dst.v6 = 0;
    memcpy(p->src.b, d + 12, 4);
    memcpy(p->dst.b, d + 16, 4);
    p->ip_csum = csum_fold(csum_add(0, d, ihl)) == 0 ? CSUM_OK : CSUM_BAD;
    uint16_t ff = rd16(d + 6);
    p->frag_offset = (ff & 0x1FFF) * 8;
    p->fragment = (ff & 0x2000) || p->frag_offset;

    size_t avail = n - ihl, seglen = total - ihl;
    if (seglen > avail) p->truncated = 1;
    if (avail > seglen) avail = seglen;  // drop Ethernet padding past the datagram's end
    if (p->frag_offset > 0) return 0;    // no transport header in a later fragment
    decode_l4(p, d + ihl, avail, seglen, !p->fragment);
    return 0;
}

static int decode_ipv6(const uint8_t *d, size_t n, Packet *p) {
    if (n < 40) { p->note = "truncated IPv6 header"; p->truncated = 1; return 0; }
    if ((d[0] >> 4) != 6) { p->note = "bad IP version"; return 0; }
    p->ip_version = 6;
    size_t plen = rd16(d + 4);
    p->ip_total = (uint16_t)(plen + 40);
    p->ttl = d[7];  // hop limit
    p->src.v6 = p->dst.v6 = 1;
    memcpy(p->src.b, d + 8, 16);
    memcpy(p->dst.b, d + 24, 16);
    p->ip_csum = CSUM_NA;  // IPv6 has no header checksum

    uint8_t next = d[6];
    size_t cap = n - 40 < plen ? n - 40 : plen;  // payload bytes we actually have
    if (n - 40 < plen) p->truncated = 1;
    size_t pos = 0;  // where we are in the payload
    // Walk the extension headers to find the transport protocol.
    for (int hops = 0; hops < 8; ++hops) {
        if (next != 0 && next != 43 && next != 60 && next != 51 && next != 44) break;
        size_t need = next == 44 ? 8 : 2;
        if (pos + need > cap) { p->note = "truncated IPv6 extension header"; p->truncated = 1; return 0; }
        size_t hl = next == 44 ? 8 : next == 51 ? ((size_t)d[40 + pos + 1] + 2) * 4 : ((size_t)d[40 + pos + 1] + 1) * 8;
        if (next == 44) {
            p->fragment = 1;
            p->frag_offset = (rd16(d + 40 + pos + 2) >> 3) * 8;
        }
        if (pos + hl > plen) { p->note = "bad IPv6 extension header"; return 0; }
        uint8_t following = d[40 + pos];
        pos += hl;
        next = following;
    }
    p->ip_proto = next;
    if (p->frag_offset > 0) return 0;
    if (pos > cap) { p->truncated = 1; return 0; }
    decode_l4(p, d + 40 + pos, cap - pos, plen - pos, !p->fragment);
    return 0;
}

static void decode_arp(const uint8_t *d, size_t n, Packet *p) {
    if (n < 28) { p->note = "truncated ARP"; p->truncated = 1; return; }
    if (rd16(d) != 1 || rd16(d + 2) != 0x0800 || d[4] != 6 || d[5] != 4) { p->note = "ARP for something other than IPv4 over Ethernet"; return; }
    p->has_arp = 1;
    p->arp_op = rd16(d + 6);
    memcpy(p->arp_sha, d + 8, 6);
    memcpy(p->arp_spa, d + 14, 4);
    memcpy(p->arp_tha, d + 18, 6);
    memcpy(p->arp_tpa, d + 24, 4);
}

int decode_packet(const uint8_t *d, uint32_t n, int linktype, Packet *p) {
    memset(p, 0, sizeof *p);
    p->caplen = n;
    p->vlan = -1;
    p->ip_csum = p->l4_csum = CSUM_NA;
    uint16_t et = 0;
    size_t off = 0;

    switch (linktype) {
        case LT_ETHERNET:
            if (n < 14) { p->note = "truncated Ethernet header"; p->truncated = 1; return 0; }
            p->has_eth = 1;
            memcpy(p->eth_dst, d, 6);
            memcpy(p->eth_src, d + 6, 6);
            et = rd16(d + 12);
            off = 14;
            for (int i = 0; i < 2 && (et == 0x8100 || et == 0x88A8); ++i) {  // VLAN tags (also stacked)
                if (n < off + 4) { p->note = "truncated VLAN tag"; p->truncated = 1; return 0; }
                if (p->vlan < 0) p->vlan = rd16(d + off) & 0x0FFF;
                et = rd16(d + off + 2);
                off += 4;
            }
            break;
        case LT_LINUX_SLL:
            if (n < 16) { p->note = "truncated cooked header"; p->truncated = 1; return 0; }
            et = rd16(d + 14);
            off = 16;
            break;
        case LT_RAW:
            if (n < 1) { p->note = "empty packet"; return 0; }
            et = (d[0] >> 4) == 6 ? 0x86DD : 0x0800;
            break;
        case LT_NULL: {
            if (n < 4) { p->note = "truncated loopback header"; p->truncated = 1; return 0; }
            uint32_t fam;
            memcpy(&fam, d, 4);
            if (fam > 0xFFFF) fam = ((fam >> 24) | ((fam >> 8) & 0xFF00) | ((fam << 8) & 0xFF0000) | (fam << 24));
            et = fam == 2 ? 0x0800 : (fam == 24 || fam == 28 || fam == 30) ? 0x86DD : 0;
            off = 4;
            break;
        }
        default:
            p->note = "unsupported link type";
            return -1;
    }
    p->ethertype = et;
    if (et == 0x0800) decode_ipv4(d + off, n - off, p);
    else if (et == 0x86DD) decode_ipv6(d + off, n - off, p);
    else if (et == 0x0806) decode_arp(d + off, n - off, p);
    else if (!p->note) p->note = "unrecognised protocol";
    return 0;
}
