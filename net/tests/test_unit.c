// Unit tests: decoding real packets (built by scapy, see gen_vectors.py), DNS names,
// the filter language, capture-file reading, and a fuzz pass over damaged input.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "decode.h"
#include "filter.h"
#include "flows.h"
#include "format.h"
#include "pcap.h"
#include "vectors.h"

static int checks, failed;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        checks++;                                                                     \
        if (!(cond)) { failed++; printf("FAIL line %d: %s\n", __LINE__, #cond); }     \
    } while (0)

#define CHECK_STR(got, want)                                                          \
    do {                                                                              \
        checks++;                                                                     \
        if (strcmp((got), (want)) != 0) {                                             \
            failed++;                                                                 \
            printf("FAIL line %d: got \"%s\", wanted \"%s\"\n", __LINE__, (got), (want)); \
        }                                                                             \
    } while (0)

// ---- helpers --------------------------------------------------------------------

static size_t from_hex(const char *hex, uint8_t *out) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; ++i) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

static uint8_t g_buf[4096];
static size_t g_len;

static Packet vec(const char *name) {
    Packet p;
    for (int i = 0; kVectors[i].name; ++i) {
        if (!strcmp(kVectors[i].name, name)) {
            g_len = from_hex(kVectors[i].hex, g_buf);
            decode_packet(g_buf, (uint32_t)g_len, LT_ETHERNET, &p);
            p.origlen = (uint32_t)g_len;
            return p;
        }
    }
    printf("missing vector %s\n", name);
    exit(2);
}

static char *addr(const Addr *a) {
    static char buf[4][64];
    static int k;
    k = (k + 1) % 4;
    addr_to_string(a, buf[k], sizeof buf[k]);
    return buf[k];
}

// ---- tests ----------------------------------------------------------------------

static void test_checksum(void) {
    // RFC 1071's worked example: these 8 bytes sum to 0xddf2, so the checksum is 0x220d.
    const uint8_t d[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
    CHECK(csum_fold(csum_add(0, d, sizeof d)) == 0x220d);
    const uint8_t odd[] = {0x01, 0x02, 0x03};  // an odd last byte is padded with a zero
    CHECK(csum_fold(csum_add(0, odd, 3)) == (uint16_t)~(0x0102 + 0x0300));
}

static void test_tcp(void) {
    Packet p = vec("tcp_syn");
    CHECK(p.has_eth && p.ethertype == 0x0800 && p.vlan == -1);
    CHECK(p.eth_src[5] == 0x0a && p.eth_dst[5] == 0x0b);
    CHECK(p.ip_version == 4 && p.ttl == 64 && p.ip_proto == 6 && p.l4 == 6);
    CHECK_STR(addr(&p.src), "10.0.0.5");
    CHECK_STR(addr(&p.dst), "93.184.216.34");
    CHECK(p.sport == 51514 && p.dport == 80);
    CHECK(p.seq == 1000 && p.ack == 0 && p.tcp_flags == TCP_SYN && p.window == 64240);
    CHECK(p.payload_len == 0 && p.ip_csum == CSUM_OK && p.l4_csum == CSUM_OK);
    CHECK(!p.truncated && p.note == NULL);

    p = vec("tcp_synack");
    CHECK(p.tcp_flags == (TCP_SYN | TCP_ACK) && p.ack == 1001 && p.ttl == 57);

    p = vec("tcp_bad_checksum");
    CHECK(p.ip_csum == CSUM_OK && p.l4_csum == CSUM_BAD);
}

static void test_http(void) {
    const char *req = "GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n";
    Packet p = vec("http_get");
    CHECK(p.http == 1);
    CHECK_STR(p.http_line, "GET /index.html HTTP/1.1");
    CHECK(p.payload_len == strlen(req) && memcmp(p.payload, req, strlen(req)) == 0);
    CHECK(p.tcp_flags == (TCP_PSH | TCP_ACK));
    p = vec("http_resp");
    CHECK(p.http == 2);
    CHECK_STR(p.http_line, "HTTP/1.1 200 OK");
    p = vec("tcp_syn");
    CHECK(p.http == 0);
}

static void test_dns(void) {
    Packet p = vec("dns_query");
    CHECK(p.l4 == 17 && p.dns.present && !p.dns.response && !p.dns.malformed);
    CHECK(p.dns.id == 0x1234 && p.dns.qdcount == 1 && p.dns.ancount == 0 && p.dns.qtype == 1);
    CHECK_STR(p.dns.qname, "www.example.com");
    CHECK(p.l4_csum == CSUM_OK);

    p = vec("dns_response");
    CHECK(p.dns.response && p.dns.rcode == 0 && p.dns.ancount == 3 && p.dns.nanswers == 3);
    CHECK_STR(p.dns.answers[0], "CNAME web.example.net");
    CHECK_STR(p.dns.answers[1], "A 192.0.2.7");
    CHECK_STR(p.dns.answers[2], "AAAA 2001:db8::7");

    p = vec("dns_nxdomain");
    CHECK(p.dns.rcode == 3 && p.dns.qtype == 15 && p.dns.ancount == 0);
    CHECK_STR(p.dns.qname, "nope.invalid");
}

static void test_dns_names(void) {
    char out[64];
    size_t next;
    // 3www7example3com0, then a name that is just a pointer back to offset 0
    const uint8_t m[] = {3, 'w', 'w', 'w', 7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0xC0, 0x00};
    CHECK(dns_read_name(m, sizeof m, 0, out, sizeof out, &next) == 0 && next == 17);
    CHECK_STR(out, "www.example.com");
    CHECK(dns_read_name(m, sizeof m, 17, out, sizeof out, &next) == 0 && next == 19);  // resumes after the pointer
    CHECK_STR(out, "www.example.com");

    const uint8_t root[] = {0};
    CHECK(dns_read_name(root, 1, 0, out, sizeof out, &next) == 0);
    CHECK_STR(out, ".");

    const uint8_t loop[] = {0xC0, 0x00};  // points at itself
    CHECK(dns_read_name(loop, 2, 0, out, sizeof out, &next) == -1);
    const uint8_t pair[] = {0xC0, 0x02, 0xC0, 0x00};  // two pointers chasing each other
    CHECK(dns_read_name(pair, 4, 0, out, sizeof out, &next) == -1);
    const uint8_t beyond[] = {0xC0, 0x7F};  // pointer past the end of the message
    CHECK(dns_read_name(beyond, 2, 0, out, sizeof out, &next) == -1);
    const uint8_t cut[] = {5, 'a', 'b'};  // label longer than what is left
    CHECK(dns_read_name(cut, 3, 0, out, sizeof out, &next) == -1);
    const uint8_t noend[] = {1, 'a'};  // no terminating zero
    CHECK(dns_read_name(noend, 2, 0, out, sizeof out, &next) == -1);
    const uint8_t badlabel[] = {0x80, 0};  // reserved label type
    CHECK(dns_read_name(badlabel, 2, 0, out, sizeof out, &next) == -1);

    char small[8];  // too small for the name: refuse rather than overflow
    CHECK(dns_read_name(m, sizeof m, 0, small, sizeof small, &next) == -1);
    CHECK(dns_read_name(m, sizeof m, 0, small, 0, &next) == -1);

    uint8_t huge[300];  // a name made of many labels: must stop at the output buffer, not run on
    size_t n = 0;
    for (int i = 0; i < 90; ++i) { huge[n++] = 2; huge[n++] = 'a'; huge[n++] = 'b'; }
    huge[n++] = 0;
    CHECK(dns_read_name(huge, n, 0, out, sizeof out, &next) == -1);
}

static void test_arp_icmp_udp(void) {
    Packet p = vec("arp_request");
    CHECK(p.has_arp && p.arp_op == 1 && !p.ip_version);
    CHECK(p.arp_spa[3] == 5 && p.arp_tpa[3] == 1);
    CHECK(p.eth_dst[0] == 0xff);
    p = vec("arp_reply");
    CHECK(p.arp_op == 2 && p.arp_sha[5] == 0x0b && p.arp_tha[5] == 0x0a);

    p = vec("icmp_echo");
    CHECK(p.l4 == 1 && p.icmp_type == 8 && p.icmp_code == 0 && p.icmp_id == 0x77 && p.icmp_seq == 3);
    CHECK(p.payload_len == 8 && memcmp(p.payload, "abcdefgh", 8) == 0 && p.l4_csum == CSUM_OK);

    p = vec("udp_plain");
    CHECK(p.l4 == 17 && p.sport == 5000 && p.dport == 6000 && p.payload_len == 9);
    CHECK(memcmp(p.payload, "hello udp", 9) == 0 && !p.dns.present && p.l4_csum == CSUM_OK);

    p = vec("udp_zero_checksum");
    CHECK(p.l4 == 17 && p.l4_csum == CSUM_NA && p.payload_len == 6);

    p = vec("ip_bad_checksum");
    CHECK(p.ip_csum == CSUM_BAD && p.l4_csum == CSUM_OK);
}

static void test_ipv6(void) {
    Packet p = vec("ipv6_tcp");
    CHECK(p.ip_version == 6 && p.ttl == 60 && p.l4 == 6 && p.sport == 44444 && p.dport == 443);
    CHECK_STR(addr(&p.src), "2001:db8::1");
    CHECK_STR(addr(&p.dst), "2001:db8::2");
    CHECK(p.tcp_flags == TCP_SYN && p.seq == 77 && p.l4_csum == CSUM_OK && p.ip_csum == CSUM_NA);

    p = vec("ipv6_icmp_echo");
    CHECK(p.l4 == 58 && p.icmp_type == 128 && p.icmp_id == 9 && p.icmp_seq == 4 && p.l4_csum == CSUM_OK);
    CHECK(p.ttl == 255);

    p = vec("ipv6_hopbyhop_udp");  // the UDP header sits behind an extension header
    CHECK(p.ip_proto == 17 && p.l4 == 17 && p.sport == 1111 && p.dport == 2222);
    CHECK(p.payload_len == 10 && memcmp(p.payload, "v6 payload", 10) == 0 && p.l4_csum == CSUM_OK);

    p = vec("ipv6_fragment_first");
    CHECK(p.fragment && p.frag_offset == 0 && p.l4 == 17 && p.sport == 1 && p.dport == 2 && p.l4_csum == CSUM_NA);
    p = vec("ipv6_fragment_later");
    CHECK(p.fragment && p.frag_offset == 40 && p.l4 == 0 && p.ip_proto == 17);
}

static void test_vlan_and_fragments(void) {
    Packet p = vec("vlan_udp");
    CHECK(p.vlan == 100 && p.ethertype == 0x0800 && p.l4 == 17 && p.sport == 7);
    CHECK_STR(addr(&p.src), "10.1.1.1");

    p = vec("ipv4_fragment_first");
    CHECK(p.fragment && p.frag_offset == 0 && p.l4 == 17 && p.sport == 9 && p.l4_csum == CSUM_NA);
    p = vec("ipv4_fragment_later");
    CHECK(p.fragment && p.frag_offset == 80 && p.l4 == 0 && p.ip_proto == 17 && p.payload_len == 0);
}

static void test_truncation(void) {
    // Cut a TCP packet at every possible length. Nothing may crash and the result must say so.
    for (const Vector *v = kVectors; v->name; ++v) {
        uint8_t buf[2048];
        size_t n = from_hex(v->hex, buf);
        for (size_t cut = 0; cut <= n; ++cut) {
            Packet p;
            decode_packet(buf, (uint32_t)cut, LT_ETHERNET, &p);
            if (cut < n && p.ip_version && p.l4) {
                // when the transport header survived but the data did not, it must not claim a checksum verdict
                CHECK(p.l4_csum == CSUM_NA || cut == n);
            }
            if (cut < 14) CHECK(!p.has_eth);
        }
    }
    uint8_t buf[2048];
    size_t n = from_hex(kVectors[2].hex, buf);  // http_get
    Packet p;
    decode_packet(buf, (uint32_t)n - 10, LT_ETHERNET, &p);
    CHECK(p.truncated && p.l4 == 6 && p.l4_csum == CSUM_NA);
    CHECK(p.payload_len == strlen("GET /index.html HTTP/1.1\r\nHost: example.com\r\n\r\n") - 10);
}

static void test_ethernet_padding(void) {
    // A 20-byte UDP datagram in a 60-byte frame: the 6+ padding bytes aren't payload.
    Packet p = vec("udp_plain");
    uint8_t padded[200];
    memcpy(padded, g_buf, g_len);
    memset(padded + g_len, 0, 20);
    Packet q;
    decode_packet(padded, (uint32_t)g_len + 20, LT_ETHERNET, &q);
    CHECK(q.payload_len == p.payload_len && q.l4_csum == CSUM_OK && !q.truncated);
}

static void test_other_linktypes(void) {
    Packet e = vec("tcp_syn");
    Packet p;
    // raw IP: the frame without its 14-byte Ethernet header
    decode_packet(g_buf + 14, (uint32_t)g_len - 14, LT_RAW, &p);
    CHECK(p.ip_version == 4 && p.l4 == 6 && p.sport == e.sport && !p.has_eth);
    // Linux cooked: 16-byte header, protocol in the last two bytes
    uint8_t sll[200] = {0};
    sll[14] = 0x08;
    sll[15] = 0x00;
    memcpy(sll + 16, g_buf + 14, g_len - 14);
    decode_packet(sll, (uint32_t)g_len - 14 + 16, LT_LINUX_SLL, &p);
    CHECK(p.ip_version == 4 && p.dport == 80);
    // BSD loopback: 4-byte family (2 = IPv4) in either byte order
    uint8_t lo[200] = {2, 0, 0, 0};
    memcpy(lo + 4, g_buf + 14, g_len - 14);
    decode_packet(lo, (uint32_t)g_len - 14 + 4, LT_NULL, &p);
    CHECK(p.ip_version == 4 && p.sport == 51514);
    uint8_t lo2[200] = {0, 0, 0, 2};
    memcpy(lo2 + 4, g_buf + 14, g_len - 14);
    decode_packet(lo2, (uint32_t)g_len - 14 + 4, LT_NULL, &p);
    CHECK(p.ip_version == 4);
    CHECK(decode_packet(g_buf, (uint32_t)g_len, 999, &p) == -1);
}

static void test_format(void) {
    char line[1024];
    Packet p = vec("tcp_syn");
    p.ts_sec = 1700000000;  // 2023-11-14 22:13:20 UTC
    p.ts_nsec = 123456000;
    format_line(&p, 0, 0, line, sizeof line);
    CHECK_STR(line, "22:13:20.123456 IP 10.0.0.5.51514 > 93.184.216.34.80: TCP [S] seq 1000 win 64240 len 0");
    format_line(&p, 1, 0, line, sizeof line);
    CHECK(strncmp(line, "1700000000.123456 IP ", 21) == 0);

    p = vec("tcp_synack");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "TCP [S.] seq 5000 ack 1001 win 65535 len 0") != NULL);

    p = vec("http_get");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "| HTTP GET /index.html HTTP/1.1") != NULL);

    p = vec("dns_response");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "DNS response id 4660 A www.example.com rcode 0 => CNAME web.example.net, A 192.0.2.7, AAAA 2001:db8::7") != NULL);

    p = vec("arp_request");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "ARP who-has 10.0.0.1 tell 10.0.0.5") != NULL);
    p = vec("arp_reply");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "ARP 10.0.0.1 is-at 02:00:00:00:00:0b") != NULL);

    p = vec("icmp_echo");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "ICMP echo request id 119 seq 3") != NULL);

    p = vec("tcp_bad_checksum");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "checksum") == NULL);  // hidden unless asked for
    format_line(&p, 0, 1, line, sizeof line);
    CHECK(strstr(line, "[bad TCP checksum]") != NULL);

    p = vec("vlan_udp");
    format_line(&p, 0, 0, line, sizeof line);
    CHECK(strstr(line, "[vlan 100]") != NULL);

    char tiny[16];  // an output buffer that is too small must truncate, not overflow
    format_line(&p, 0, 0, tiny, sizeof tiny);
    CHECK(strlen(tiny) < sizeof tiny);
}

static int matches(const char *expr, const char *vector) {
    char err[200] = "";
    Filter *f = filter_compile(expr, err, sizeof err);
    if (!f) {
        printf("  filter '%s' failed to compile: %s\n", expr, err);
        return -1;
    }
    Packet p = vec(vector);
    int r = filter_match(f, &p);
    filter_free(f);
    return r;
}

static int compile_fails(const char *expr, const char *needle) {
    char err[200] = "";
    Filter *f = filter_compile(expr, err, sizeof err);
    if (f) {
        filter_free(f);
        return 0;
    }
    return strstr(err, needle) != NULL;
}

static void test_filter(void) {
    CHECK(matches("", "tcp_syn") == 1);
    CHECK(matches("tcp", "tcp_syn") == 1 && matches("udp", "tcp_syn") == 0);
    CHECK(matches("udp", "dns_query") == 1 && matches("dns", "dns_query") == 1 && matches("dns", "udp_plain") == 0);
    CHECK(matches("http", "http_get") == 1 && matches("http", "tcp_syn") == 0);
    CHECK(matches("ip", "tcp_syn") == 1 && matches("ip6", "tcp_syn") == 0 && matches("ip6", "ipv6_tcp") == 1);
    CHECK(matches("arp", "arp_request") == 1 && matches("ip", "arp_request") == 0);
    CHECK(matches("icmp", "icmp_echo") == 1 && matches("icmp6", "ipv6_icmp_echo") == 1);

    CHECK(matches("port 80", "tcp_syn") == 1 && matches("port 81", "tcp_syn") == 0);
    CHECK(matches("src port 51514", "tcp_syn") == 1 && matches("dst port 51514", "tcp_syn") == 0);
    CHECK(matches("dst port 80", "tcp_syn") == 1);
    CHECK(matches("port 80", "icmp_echo") == 0);
    CHECK(matches("host 10.0.0.5", "tcp_syn") == 1 && matches("host 10.0.0.6", "tcp_syn") == 0);
    CHECK(matches("src host 10.0.0.5", "tcp_syn") == 1 && matches("dst host 10.0.0.5", "tcp_syn") == 0);
    CHECK(matches("host 2001:db8::1", "ipv6_tcp") == 1 && matches("host 10.0.0.5", "ipv6_tcp") == 0);
    CHECK(matches("net 10.0.0.0/24", "tcp_syn") == 1 && matches("net 10.0.1.0/24", "tcp_syn") == 0);
    CHECK(matches("net 93.184.0.0/16", "tcp_syn") == 1 && matches("dst net 93.0.0.0/8", "tcp_syn") == 1);
    CHECK(matches("src net 93.0.0.0/8", "tcp_syn") == 0);
    CHECK(matches("net 10.0.0.0/13", "tcp_syn") == 1);   // a prefix that doesn't end on a byte boundary
    CHECK(matches("net 10.8.0.0/13", "tcp_syn") == 0);
    CHECK(matches("net 2001:db8::/32", "ipv6_tcp") == 1 && matches("net 2001:db9::/32", "ipv6_tcp") == 0);
    CHECK(matches("host 10.0.0.1", "arp_request") == 1);  // ARP carries addresses too

    CHECK(matches("syn", "tcp_syn") == 1 && matches("syn", "tcp_synack") == 0);
    CHECK(matches("greater 100", "http_get") == 1 && matches("greater 100", "tcp_syn") == 0);
    CHECK(matches("less 60", "tcp_syn") == 1 && matches("less 50", "tcp_syn") == 0);
    CHECK(matches("vlan", "vlan_udp") == 1 && matches("vlan", "tcp_syn") == 0);
    CHECK(matches("badcsum", "tcp_bad_checksum") == 1 && matches("badcsum", "ip_bad_checksum") == 1 && matches("badcsum", "tcp_syn") == 0);

    CHECK(matches("tcp and port 80", "tcp_syn") == 1 && matches("tcp and port 81", "tcp_syn") == 0);
    CHECK(matches("udp or tcp", "tcp_syn") == 1);
    CHECK(matches("not tcp", "tcp_syn") == 0 && matches("! tcp", "udp_plain") == 1 && matches("!tcp", "udp_plain") == 1);
    CHECK(matches("not not tcp", "tcp_syn") == 1);
    CHECK(matches("tcp && port 80", "tcp_syn") == 1 && matches("udp || port 80", "tcp_syn") == 1);
    // precedence: and binds tighter than or;  a or b and c  ==  a or (b and c)
    CHECK(matches("udp or tcp and port 81", "tcp_syn") == 0);
    CHECK(matches("(udp or tcp) and port 80", "tcp_syn") == 1);
    CHECK(matches("(udp or tcp) and not port 80", "tcp_syn") == 0);
    CHECK(matches("tcp and (port 80 or port 443) and not syn", "http_get") == 1);
    CHECK(matches("(tcp)", "tcp_syn") == 1 && matches("((tcp))", "tcp_syn") == 1);
    CHECK(matches("tcp and(port 80)", "tcp_syn") == 1);

    CHECK(compile_fails("tcp and", "ends too early"));
    CHECK(compile_fails("and tcp", "unknown filter word"));
    CHECK(compile_fails("(tcp", "closing parenthesis"));
    CHECK(compile_fails("tcp)", "unexpected"));
    CHECK(compile_fails("port", "expected a port"));
    CHECK(compile_fails("port abc", "expected a number"));
    CHECK(compile_fails("port 70000", "expected a number"));
    CHECK(compile_fails("host 300.1.1.1", "not an IP address"));
    CHECK(compile_fails("host 10.0.0.1/8", "single address"));
    CHECK(compile_fails("net 10.0.0.0/40", "expected a number"));
    CHECK(compile_fails("src", "expected host"));
    CHECK(compile_fails("src tcp", "unknown filter word"));
    CHECK(compile_fails("bogus", "unknown filter word 'bogus'"));
    CHECK(compile_fails("tcp udp", "unexpected 'udp'"));
    CHECK(compile_fails("greater", "expected a length"));
}

// ---- capture files --------------------------------------------------------------

typedef struct { uint8_t b[4096]; size_t n; } Bytes;

static void put(Bytes *o, const void *d, size_t n) { memcpy(o->b + o->n, d, n); o->n += n; }
static void p16(Bytes *o, uint16_t v, int be) { uint8_t x[2] = {be ? (uint8_t)(v >> 8) : (uint8_t)v, be ? (uint8_t)v : (uint8_t)(v >> 8)}; put(o, x, 2); }
static void p32(Bytes *o, uint32_t v, int be) {
    uint8_t x[4];
    for (int i = 0; i < 4; ++i) x[i] = (uint8_t)(v >> (be ? 24 - 8 * i : 8 * i));
    put(o, x, 4);
}

static void write_file(const char *path, const Bytes *b) {
    FILE *f = fopen(path, "wb");
    fwrite(b->b, 1, b->n, f);
    fclose(f);
}

static const char *tmp_path(void) {
    static char path[64];
    snprintf(path, sizeof path, "/tmp/pktscope-test-%d.cap", (int)getpid());
    return path;
}

static void test_pcap_classic(void) {
    const uint8_t pkt[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    for (int variant = 0; variant < 4; ++variant) {  // little/big endian x micro/nano
        int be = variant & 1, nano = variant >> 1;
        Bytes b = {{0}, 0};
        p32(&b, nano ? 0xA1B23C4D : 0xA1B2C3D4, be);
        p16(&b, 2, be); p16(&b, 4, be);
        p32(&b, 0, be); p32(&b, 0, be); p32(&b, 65535, be); p32(&b, LT_ETHERNET, be);
        p32(&b, 1700000000, be); p32(&b, 250000, be); p32(&b, sizeof pkt, be); p32(&b, 60, be);
        put(&b, pkt, sizeof pkt);
        p32(&b, 1700000001, be); p32(&b, 5, be); p32(&b, 0, be); p32(&b, 0, be);  // an empty packet
        write_file(tmp_path(), &b);

        PcapReader r;
        char err[200];
        PcapRecord rec;
        CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == 0);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 1);
        CHECK(rec.ts_sec == 1700000000 && rec.ts_nsec == (nano ? 250000u : 250000000u));
        CHECK(rec.caplen == sizeof pkt && rec.origlen == 60 && rec.linktype == LT_ETHERNET);
        CHECK(memcmp(rec.data, pkt, sizeof pkt) == 0);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 1 && rec.caplen == 0);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 0);
        pcap_close(&r);
    }
}

static void test_pcap_errors(void) {
    PcapReader r;
    char err[200];
    PcapRecord rec;
    Bytes b = {{0}, 0};
    CHECK(pcap_open(&r, "/nonexistent/file.pcap", err, sizeof err) == -1);

    b.n = 0; p32(&b, 0x12345678, 0);
    write_file(tmp_path(), &b);
    CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == -1 && strstr(err, "magic"));

    b.n = 0; p32(&b, 0xA1B2C3D4, 0); p16(&b, 2, 0);  // header cut off
    write_file(tmp_path(), &b);
    CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == -1 && strstr(err, "cut off"));

    // a packet claiming to be 2 GB long
    b.n = 0;
    p32(&b, 0xA1B2C3D4, 0); p16(&b, 2, 0); p16(&b, 4, 0); p32(&b, 0, 0); p32(&b, 0, 0); p32(&b, 65535, 0); p32(&b, 1, 0);
    p32(&b, 1, 0); p32(&b, 1, 0); p32(&b, 0x7FFFFFFF, 0); p32(&b, 0x7FFFFFFF, 0);
    write_file(tmp_path(), &b);
    CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == 0);
    CHECK(pcap_next(&r, &rec, err, sizeof err) == -1 && strstr(err, "damaged"));
    pcap_close(&r);

    // a packet that stops short
    b.n = 0;
    p32(&b, 0xA1B2C3D4, 0); p16(&b, 2, 0); p16(&b, 4, 0); p32(&b, 0, 0); p32(&b, 0, 0); p32(&b, 65535, 0); p32(&b, 1, 0);
    p32(&b, 1, 0); p32(&b, 1, 0); p32(&b, 50, 0); p32(&b, 50, 0);
    for (int i = 0; i < 20; ++i) put(&b, "x", 1);
    write_file(tmp_path(), &b);
    CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == 0);
    CHECK(pcap_next(&r, &rec, err, sizeof err) == -1 && strstr(err, "middle of a packet"));
    pcap_close(&r);
}

static void ng_block(Bytes *o, uint32_t type, const Bytes *body, int be) {
    uint32_t pad = (4 - body->n % 4) % 4;
    uint32_t total = (uint32_t)(12 + body->n + pad);
    p32(o, type, be);
    p32(o, total, be);
    put(o, body->b, body->n);
    for (uint32_t i = 0; i < pad; ++i) put(o, "\0", 1);
    p32(o, total, be);
}

static void test_pcapng(void) {
    const uint8_t pkt[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};  // 5 bytes: needs 3 bytes of padding
    for (int be = 0; be < 2; ++be) {
        Bytes f = {{0}, 0}, body = {{0}, 0};
        p32(&body, 0x1A2B3C4D, be); p16(&body, 1, be); p16(&body, 0, be); p32(&body, 0xFFFFFFFF, be); p32(&body, 0xFFFFFFFF, be);
        ng_block(&f, 0x0A0D0D0A, &body, be);

        body.n = 0;  // interface 0: Ethernet, with if_tsresol = 9 (nanoseconds)
        p16(&body, LT_ETHERNET, be); p16(&body, 0, be); p32(&body, 65535, be);
        p16(&body, 9, be); p16(&body, 1, be); put(&body, "\x09\0\0\0", 4);
        p16(&body, 0, be); p16(&body, 0, be);
        ng_block(&f, 1, &body, be);

        body.n = 0;  // interface 1: raw IP, default microsecond resolution
        p16(&body, LT_RAW, be); p16(&body, 0, be); p32(&body, 65535, be);
        ng_block(&f, 1, &body, be);

        body.n = 0;  // a packet on interface 0 at 1.5 s past the epoch (in nanoseconds)
        uint64_t ns = 1500000000ULL;
        p32(&body, 0, be); p32(&body, (uint32_t)(ns >> 32), be); p32(&body, (uint32_t)ns, be);
        p32(&body, sizeof pkt, be); p32(&body, 99, be); put(&body, pkt, sizeof pkt);
        ng_block(&f, 6, &body, be);

        body.n = 0;  // a block type we don't understand: skipped
        p32(&body, 0, be);
        ng_block(&f, 5, &body, be);

        body.n = 0;  // a packet on interface 1 at 2.000001 s (in microseconds)
        uint64_t us = 2000001ULL;
        p32(&body, 1, be); p32(&body, (uint32_t)(us >> 32), be); p32(&body, (uint32_t)us, be);
        p32(&body, sizeof pkt, be); p32(&body, sizeof pkt, be); put(&body, pkt, sizeof pkt);
        ng_block(&f, 6, &body, be);
        write_file(tmp_path(), &f);

        PcapReader r;
        char err[200];
        PcapRecord rec;
        CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == 0);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 1);
        CHECK(rec.ts_sec == 1 && rec.ts_nsec == 500000000u && rec.linktype == LT_ETHERNET);
        CHECK(rec.caplen == 5 && rec.origlen == 99 && memcmp(rec.data, pkt, 5) == 0);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 1);
        CHECK(rec.ts_sec == 2 && rec.ts_nsec == 1000u && rec.linktype == LT_RAW);
        CHECK(pcap_next(&r, &rec, err, sizeof err) == 0);
        pcap_close(&r);
    }
}

static void test_pcap_roundtrip_with_writer(void) {
    FILE *f = fopen(tmp_path(), "wb");
    CHECK(pcap_write_header(f, LT_ETHERNET, 65535) == 0);
    uint8_t buf[2048];
    size_t n = from_hex(kVectors[0].hex, buf);
    CHECK(pcap_write_record(f, 1700000000, 42, buf, (uint32_t)n, (uint32_t)n) == 0);
    fclose(f);
    PcapReader r;
    char err[200];
    PcapRecord rec;
    CHECK(pcap_open(&r, tmp_path(), err, sizeof err) == 0);
    CHECK(pcap_next(&r, &rec, err, sizeof err) == 1 && rec.ts_nsec == 42000u && rec.caplen == n);
    Packet p;
    decode_packet(rec.data, rec.caplen, rec.linktype, &p);
    CHECK(p.l4 == 6 && p.dport == 80);
    pcap_close(&r);
}

static void test_stats(void) {
    Stats *s = stats_new();
    const char *names[] = {"tcp_syn", "tcp_synack", "http_get", "http_resp", "dns_query", "dns_response", "arp_request", "icmp_echo"};
    for (size_t i = 0; i < sizeof names / sizeof *names; ++i) {
        Packet p = vec(names[i]);
        p.ts_sec = 100 + (int64_t)i;
        stats_add(s, &p);
    }
    char *text = NULL;
    size_t size = 0;
    FILE *m = open_memstream(&text, &size);
    stats_print(s, m, 10);
    fclose(m);
    CHECK(strstr(text, "8 packets") != NULL);
    CHECK(strstr(text, "TCP                4") != NULL);
    CHECK(strstr(text, "UDP                2") != NULL);
    CHECK(strstr(text, "10.0.0.5.51514 <-> 93.184.216.34.80") != NULL);
    CHECK(strstr(text, "GET /index.html HTTP/1.1") != NULL);
    CHECK(strstr(text, "A www.example.com") != NULL);
    CHECK(strstr(text, "open") != NULL);  // the TCP conversation was established, not closed
    free(text);
    stats_free(s);
}

// ---- fuzzing --------------------------------------------------------------------

static uint32_t rng_state = 12345;
static uint32_t rng(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

static void test_fuzz(void) {
    // Damage real packets at random and push them through every stage. The checks here are
    // implicit: under the sanitizers (make sanitize) any out-of-bounds read aborts the run.
    FILE *devnull = fopen("/dev/null", "w");
    Filter *f = filter_compile("(tcp and port 80) or dns or net 10.0.0.0/8 or host 2001:db8::1", NULL, 0);
    Stats *stats = stats_new();
    int count = 0;
    for (const Vector *v = kVectors; v->name; ++v) {
        uint8_t orig[2048], buf[2048];
        size_t n = from_hex(v->hex, orig);
        for (int iter = 0; iter < 4000; ++iter) {
            size_t len = n;
            memcpy(buf, orig, n);
            int edits = 1 + (int)(rng() % 6);
            for (int e = 0; e < edits; ++e) buf[rng() % n] = (uint8_t)rng();
            if (rng() % 3 == 0) len = rng() % (n + 1);  // also cut it short
            Packet p;
            decode_packet(buf, (uint32_t)len, (int)(rng() % 3 == 0 ? LT_RAW : LT_ETHERNET), &p);
            p.origlen = (uint32_t)n;
            char line[1024];
            format_line(&p, (int)(iter & 1), 1, line, sizeof line);
            format_json(&p, devnull);
            if (p.payload_len) format_hex(p.payload, p.payload_len, 64, devnull);
            filter_match(f, &p);
            stats_add(stats, &p);
            count++;
        }
    }
    // pure noise too
    for (int iter = 0; iter < 20000; ++iter) {
        uint8_t buf[300];
        size_t len = rng() % sizeof buf;
        for (size_t i = 0; i < len; ++i) buf[i] = (uint8_t)rng();
        if (len > 13 && (rng() & 1)) { buf[12] = 0x08; buf[13] = (rng() & 1) ? 0x00 : 0x06; }
        Packet p;
        decode_packet(buf, (uint32_t)len, LT_ETHERNET, &p);
        char line[1024];
        format_line(&p, 0, 1, line, sizeof line);
        format_json(&p, devnull);
        count++;
    }
    fclose(devnull);
    filter_free(f);
    stats_free(stats);
    CHECK(count > 100000);
}

int main(void) {
    test_checksum();
    test_tcp();
    test_http();
    test_dns();
    test_dns_names();
    test_arp_icmp_udp();
    test_ipv6();
    test_vlan_and_fragments();
    test_truncation();
    test_ethernet_padding();
    test_other_linktypes();
    test_format();
    test_filter();
    test_pcap_classic();
    test_pcap_errors();
    test_pcapng();
    test_pcap_roundtrip_with_writer();
    test_stats();
    test_fuzz();
    unlink(tmp_path());
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
