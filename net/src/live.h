// Capturing from a network interface (Linux, needs root or CAP_NET_RAW).
#ifndef LIVE_H
#define LIVE_H

#include <stddef.h>
#include <stdint.h>

// Returns a socket (>= 0) or -1 with a message in `err`. *linktype is set to LT_ETHERNET.
int live_open(const char *iface, int *linktype, char *err, size_t errsz);
// 1 = a packet (length in *len, time in sec/nsec), 0 = timed out, -1 = error.
int live_read(int fd, uint8_t *buf, size_t cap, uint32_t *len, uint32_t *orig, int64_t *sec, uint32_t *nsec, int timeout_ms);
void live_close(int fd);

#endif
