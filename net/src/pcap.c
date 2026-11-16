#include "pcap.h"

#include <stdlib.h>
#include <string.h>

#define MAX_RECORD (16u * 1024 * 1024)  // refuse absurd lengths from a damaged file

static void fail(char *err, size_t n, const char *msg) {
    if (err && n) snprintf(err, n, "%s", msg);
}

static uint32_t sw32(uint32_t v) {
    return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}
static uint16_t sw16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

static uint32_t u32(const PcapReader *r, const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return r->swap ? sw32(v) : v;
}
static uint16_t u16(const PcapReader *r, const uint8_t *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return r->swap ? sw16(v) : v;
}

static int ensure(PcapReader *r, size_t n) {
    if (n > r->cap) {
        uint8_t *b = realloc(r->buf, n);
        if (!b) return -1;
        r->buf = b;
        r->cap = n;
    }
    return 0;
}

int pcap_open(PcapReader *r, const char *path, char *err, size_t errsz) {
    memset(r, 0, sizeof *r);
    r->f = fopen(path, "rb");
    if (!r->f) {
        snprintf(err, errsz, "%s: cannot open", path);
        return -1;
    }
    uint8_t h[24];
    size_t got = fread(h, 1, 4, r->f);
    if (got < 4) {
        fail(err, errsz, "not a capture file: too short");
        goto bad;
    }
    uint32_t magic;
    memcpy(&magic, h, 4);
    if (magic == 0xA1B2C3D4u || magic == 0xA1B23C4Du) {
        r->nano = magic == 0xA1B23C4Du;
    } else if (magic == 0xD4C3B2A1u || magic == 0x4D3CB2A1u) {
        r->swap = 1;
        r->nano = magic == 0x4D3CB2A1u;
    } else if (magic == 0x0A0D0D0Au) {
        r->ng = 1;
        rewind(r->f);
        return 0;  // the section header is read by pcap_next
    } else {
        fail(err, errsz, "not a capture file (unrecognised magic number)");
        goto bad;
    }
    if (fread(h + 4, 1, 20, r->f) != 20) {
        fail(err, errsz, "capture header is cut off");
        goto bad;
    }
    r->linktype = (int)u32(r, h + 20);
    return 0;
bad:
    fclose(r->f);
    r->f = NULL;
    return -1;
}

static int read_exact(PcapReader *r, uint8_t *dst, size_t n) {
    return fread(dst, 1, n, r->f) == n ? 0 : -1;
}

static int next_classic(PcapReader *r, PcapRecord *rec, char *err, size_t errsz) {
    uint8_t h[16];
    size_t got = fread(h, 1, 16, r->f);
    if (got == 0) return 0;
    if (got < 16) {
        fail(err, errsz, "file ends in the middle of a packet header");
        return -1;
    }
    uint32_t sec = u32(r, h), frac = u32(r, h + 4), caplen = u32(r, h + 8), orig = u32(r, h + 12);
    if (caplen > MAX_RECORD) {
        fail(err, errsz, "packet length is implausibly large: the file is damaged");
        return -1;
    }
    if (ensure(r, caplen ? caplen : 1) != 0) {
        fail(err, errsz, "out of memory");
        return -1;
    }
    if (read_exact(r, r->buf, caplen) != 0) {
        fail(err, errsz, "file ends in the middle of a packet");
        return -1;
    }
    rec->ts_sec = sec;
    rec->ts_nsec = r->nano ? frac : frac * 1000u;
    if (rec->ts_nsec >= 1000000000u) rec->ts_nsec = 999999999u;  // keep a bad fraction printable
    rec->caplen = caplen;
    rec->origlen = orig;
    rec->linktype = r->linktype;
    rec->data = r->buf;
    return 1;
}

// pcapng: a sequence of blocks, each "type, total length, body, total length again".
static int next_ng(PcapReader *r, PcapRecord *rec, char *err, size_t errsz) {
    for (;;) {
        uint8_t h[8];
        size_t got = fread(h, 1, 8, r->f);
        if (got == 0) return 0;
        if (got < 8) {
            fail(err, errsz, "file ends in the middle of a block header");
            return -1;
        }
        uint32_t type_raw;
        memcpy(&type_raw, h, 4);
        if (type_raw == 0x0A0D0D0Au) {
            // Section header: its byte-order magic tells us how to read everything after it.
            uint8_t bom[4];
            if (read_exact(r, bom, 4) != 0) {
                fail(err, errsz, "section header is cut off");
                return -1;
            }
            uint32_t m;
            memcpy(&m, bom, 4);
            if (m == 0x1A2B3C4Du) r->swap = 0;
            else if (m == 0x4D3C2B1Au) r->swap = 1;
            else {
                fail(err, errsz, "bad pcapng byte-order magic");
                return -1;
            }
            uint32_t total = u32(r, h + 4);
            if (total < 28 || total > MAX_RECORD || total % 4) {
                fail(err, errsz, "bad section header length");
                return -1;
            }
            if (fseek(r->f, (long)total - 12, SEEK_CUR) != 0) {
                fail(err, errsz, "section header is cut off");
                return -1;
            }
            r->niface = 0;  // interfaces are numbered per section
            continue;
        }
        uint32_t type = u32(r, h), total = u32(r, h + 4);
        if (total < 12 || total > MAX_RECORD || total % 4) {
            fail(err, errsz, "bad block length: the file is damaged");
            return -1;
        }
        uint32_t body = total - 12;
        if (ensure(r, body ? body : 1) != 0) {
            fail(err, errsz, "out of memory");
            return -1;
        }
        if (read_exact(r, r->buf, body) != 0) {
            fail(err, errsz, "file ends in the middle of a block");
            return -1;
        }
        uint8_t trailer[4];
        if (read_exact(r, trailer, 4) != 0 || u32(r, trailer) != total) {
            fail(err, errsz, "block length mismatch: the file is damaged");
            return -1;
        }

        if (type == 1) {  // interface description
            if (body < 8 || r->niface >= PCAP_MAX_IFACES) continue;
            PcapIface *ifc = &r->ifaces[r->niface++];
            ifc->linktype = u16(r, r->buf);
            ifc->tsresol_ns = 1000;  // the default resolution is a microsecond
            for (uint32_t o = 8; o + 4 <= body;) {  // options: code, length, value padded to 4
                uint16_t code = u16(r, r->buf + o), len = u16(r, r->buf + o + 2);
                if (code == 0) break;
                if (o + 4 + len > body) break;
                if (code == 9 && len >= 1) {  // if_tsresol: ticks per second as 10^exp (or 2^exp)
                    uint8_t v = r->buf[o + 4];
                    int exp = v & 0x7F;
                    if ((v & 0x80) || exp > 9) {
                        ifc->tsresol_ns = 0;  // finer than a nanosecond, or binary: not supported
                    } else {
                        int ns = 1;
                        for (int i = exp; i < 9; ++i) ns *= 10;
                        ifc->tsresol_ns = ns;
                    }
                }
                o += 4 + ((len + 3u) & ~3u);
            }
        } else if (type == 6) {  // enhanced packet
            if (body < 20) {
                fail(err, errsz, "enhanced packet block is too short");
                return -1;
            }
            uint32_t id = u32(r, r->buf), hi = u32(r, r->buf + 4), lo = u32(r, r->buf + 8);
            uint32_t caplen = u32(r, r->buf + 12), orig = u32(r, r->buf + 16);
            if (id >= (uint32_t)r->niface) {
                fail(err, errsz, "packet refers to an interface that was never described");
                return -1;
            }
            if (caplen > body - 20) {
                fail(err, errsz, "packet is longer than its block");
                return -1;
            }
            if (r->ifaces[id].tsresol_ns == 0) {
                fail(err, errsz, "unsupported timestamp resolution");
                return -1;
            }
            uint64_t ticks = ((uint64_t)hi << 32) | lo;
            uint64_t per = (uint64_t)r->ifaces[id].tsresol_ns;
            rec->ts_sec = (int64_t)((ticks * per) / 1000000000ULL);
            rec->ts_nsec = (uint32_t)((ticks * per) % 1000000000ULL);
            rec->caplen = caplen;
            rec->origlen = orig;
            rec->linktype = r->ifaces[id].linktype;
            rec->data = r->buf + 20;
            return 1;
        } else if (type == 3) {  // simple packet: no timestamp
            if (body < 4 || r->niface < 1) continue;
            uint32_t orig = u32(r, r->buf);
            uint32_t caplen = body - 4 < orig ? body - 4 : orig;
            rec->ts_sec = 0;
            rec->ts_nsec = 0;
            rec->caplen = caplen;
            rec->origlen = orig;
            rec->linktype = r->ifaces[0].linktype;
            rec->data = r->buf + 4;
            return 1;
        }
        // any other block type (statistics, name resolution, ...) is skipped
    }
}

int pcap_next(PcapReader *r, PcapRecord *rec, char *err, size_t errsz) {
    return r->ng ? next_ng(r, rec, err, errsz) : next_classic(r, rec, err, errsz);
}

void pcap_close(PcapReader *r) {
    if (r->f) fclose(r->f);
    free(r->buf);
    memset(r, 0, sizeof *r);
}

int pcap_write_header(FILE *f, int linktype, uint32_t snaplen) {
    uint32_t h[6] = {0xA1B2C3D4u, 0, 0, 0, snaplen, (uint32_t)linktype};
    uint16_t ver[2] = {2, 4};
    memcpy(&h[1], ver, 4);
    return fwrite(h, 4, 6, f) == 6 ? 0 : -1;
}

int pcap_write_record(FILE *f, int64_t sec, uint32_t usec, const uint8_t *data, uint32_t caplen, uint32_t origlen) {
    uint32_t h[4] = {(uint32_t)sec, usec, caplen, origlen};
    if (fwrite(h, 4, 4, f) != 4) return -1;
    return fwrite(data, 1, caplen, f) == caplen ? 0 : -1;
}
