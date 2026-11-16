// Reading and writing capture files: classic pcap (both byte orders, microsecond
// and nanosecond timestamps) and the basics of pcapng.
#ifndef PCAP_H
#define PCAP_H

#include <stdint.h>
#include <stdio.h>

// Link-layer types we can decode (the numbers come from the pcap format).
enum { LT_NULL = 0, LT_ETHERNET = 1, LT_RAW = 101, LT_LINUX_SLL = 113 };

typedef struct {
    int64_t ts_sec;
    uint32_t ts_nsec;
    uint32_t caplen;   // bytes stored in the file
    uint32_t origlen;  // bytes that were on the wire
    int linktype;
    const uint8_t *data;  // valid until the next call
} PcapRecord;

typedef struct {
    int linktype;
    int tsresol_ns;  // nanoseconds per timestamp tick, 1 for nanosecond files
} PcapIface;

#define PCAP_MAX_IFACES 16

typedef struct {
    FILE *f;
    int ng;     // pcapng rather than classic pcap
    int swap;   // file byte order differs from ours
    int nano;   // classic pcap with nanosecond timestamps
    int linktype;
    PcapIface ifaces[PCAP_MAX_IFACES];
    int niface;
    uint8_t *buf;
    size_t cap;
    uint8_t pre[4];  // bytes read ahead by pcap_open and not yet consumed
    int npre;
} PcapReader;

// 0 on success; -1 with a message in `err`.
int pcap_open(PcapReader *r, const char *path, char *err, size_t errsz);
// 1 = got a record, 0 = end of file, -1 = damaged file (message in `err`).
int pcap_next(PcapReader *r, PcapRecord *rec, char *err, size_t errsz);
void pcap_close(PcapReader *r);

// Writing (classic pcap, microsecond timestamps, native byte order).
int pcap_write_header(FILE *f, int linktype, uint32_t snaplen);
int pcap_write_record(FILE *f, int64_t sec, uint32_t usec, const uint8_t *data, uint32_t caplen, uint32_t origlen);

#endif
