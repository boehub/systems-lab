// Taking a captured frame apart, layer by layer: Ethernet, ARP, IPv4/IPv6,
// TCP, UDP, ICMP, and the application protocols DNS and HTTP.
//
// Every read is bounds-checked against the bytes actually captured, so a
// truncated or hostile packet produces a partial result with a `note`, never a
// read past the end.
#ifndef DECODE_H
#define DECODE_H

#include <stddef.h>
#include <stdint.h>

#include "pcap.h"

typedef struct {
    uint8_t b[16];
    uint8_t v6;  // 0: the first 4 bytes are an IPv4 address; 1: all 16 are IPv6
} Addr;

enum { CSUM_NA = -1, CSUM_BAD = 0, CSUM_OK = 1 };

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10
#define TCP_URG 0x20

#define DNS_MAX_ANSWERS 4

typedef struct {
    int present;
    int malformed;
    uint16_t id;
    int response, opcode, rcode;
    uint16_t qdcount, ancount, nscount, arcount;
    char qname[256];
    uint16_t qtype;
    int nanswers;  // how many of the answers below were decoded
    char answers[DNS_MAX_ANSWERS][100];
} DnsInfo;

typedef struct {
    int64_t ts_sec;
    uint32_t ts_nsec;
    uint32_t caplen, origlen;

    // Ethernet
    int has_eth;
    uint8_t eth_src[6], eth_dst[6];
    uint16_t ethertype;
    int vlan;  // VLAN id, or -1

    // ARP
    int has_arp;
    uint16_t arp_op;
    uint8_t arp_sha[6], arp_tha[6], arp_spa[4], arp_tpa[4];

    // IP
    int ip_version;  // 0, 4 or 6
    Addr src, dst;
    uint8_t ttl, ip_proto;
    uint16_t ip_total;  // IPv4 total length / IPv6 payload length + 40
    int fragment;       // 1 when this is a piece of a fragmented datagram
    int frag_offset;    // in bytes
    int frag_more;      // the "more fragments" flag
    uint32_t frag_id;   // identifies the datagram the piece belongs to
    uint8_t frag_proto; // the protocol the reassembled data carries (IPv4: ip_proto)
    const uint8_t *frag_data;  // this fragment's share of the datagram, inside the captured buffer
    uint32_t frag_len;
    const uint8_t *ip_hdr;     // the IP header as captured (IPv4: 20 bytes plus options; IPv6: 40)
    uint8_t ip_hdr_len;
    int reasm_frags;    // > 0 on a datagram put back together from this many fragments
    int ip_csum;        // CSUM_*

    // Transport
    int l4;  // 6 TCP, 17 UDP, 1 ICMP, 58 ICMPv6, or 0
    uint16_t sport, dport;
    uint32_t seq, ack;
    uint8_t tcp_flags;
    uint16_t window;
    uint8_t icmp_type, icmp_code;
    uint16_t icmp_id, icmp_seq;
    int l4_csum;  // CSUM_*

    const uint8_t *payload;  // application bytes (inside the captured buffer)
    uint32_t payload_len;

    // Application
    DnsInfo dns;
    int http;  // 0 none, 1 request, 2 response
    char http_line[160];

    int truncated;     // the capture stops before the packet does
    const char *note;  // why decoding stopped early, or NULL
} Packet;

// Returns 0 when at least the link layer was understood, -1 when nothing was.
int decode_packet(const uint8_t *data, uint32_t caplen, int linktype, Packet *p);

// "1.2.3.4" or "2001:db8::1".
void addr_to_string(const Addr *a, char *out, size_t n);
// Parse an IPv4 or IPv6 address; returns 0 on success.
int addr_parse(const char *text, Addr *a);

// Internet checksum helpers (exposed for the tests).
uint32_t csum_add(uint32_t sum, const uint8_t *data, size_t len);
uint16_t csum_fold(uint32_t sum);

// Decode a DNS name starting at msg[off], following compression pointers.
// Returns 0 and the offset just past the name (in the original position) in
// *next, or -1 when the name is malformed (loops, overruns, over-long labels).
int dns_read_name(const uint8_t *msg, size_t len, size_t off, char *out, size_t outsz, size_t *next);

const char *dns_type_name(uint16_t type);

#endif
