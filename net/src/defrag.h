// Putting IP fragments back together.
//
// A datagram too big for the link is sent in pieces. Only the first piece has the
// transport header, so the pieces cannot be read until they are joined. Feed every
// fragment to defrag_add(); when the last missing piece arrives it hands back the
// complete datagram (a bare IP packet, link type RAW) ready for decode_packet().
#ifndef DEFRAG_H
#define DEFRAG_H

#include <stddef.h>
#include <stdint.h>

#include "decode.h"

typedef struct Defrag Defrag;

Defrag *defrag_new(void);
void defrag_free(Defrag *d);

// Offer one fragment (p->fragment must be set and p->frag_data valid). Returns 1 when this
// fragment completes a datagram: *out is then a malloc'd IP packet of *len bytes, which the
// caller frees, and *nfrags says how many pieces went into it. Otherwise returns 0.
int defrag_add(Defrag *d, const Packet *p, uint8_t **out, size_t *len, int *nfrags);

// Datagrams still waiting for pieces.
size_t defrag_pending(const Defrag *d);
// Datagrams given up on (the table was full, or the pieces were inconsistent).
size_t defrag_dropped(const Defrag *d);

#endif
