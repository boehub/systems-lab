// A small packet filter language, modelled on tcpdump's:
//
//   tcp and port 443          udp or icmp           not arp
//   host 10.0.0.5             src host 10.0.0.5     dst net 192.168.0.0/16
//   dns                       http                  syn   fin   rst
//   greater 1000              less 100              vlan  badcsum
//
// combined with `and` / `or` / `not` (also && || !) and parentheses.
#ifndef FILTER_H
#define FILTER_H

#include <stddef.h>

#include "decode.h"

typedef struct Filter Filter;

// Returns NULL and fills `err` when the expression is invalid. An empty
// expression compiles to a filter that matches every packet.
Filter *filter_compile(const char *expr, char *err, size_t errsz);
int filter_match(const Filter *f, const Packet *p);
void filter_free(Filter *f);

#endif
