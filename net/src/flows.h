// Keeping score: which conversations are in a capture, how big, and how they ended.
#ifndef FLOWS_H
#define FLOWS_H

#include <stdio.h>

#include "decode.h"

typedef struct Stats Stats;

Stats *stats_new(void);
void stats_free(Stats *s);
void stats_add(Stats *s, const Packet *p);
// Print totals, a protocol breakdown, the busiest conversations, DNS names asked
// for and HTTP requests seen.
void stats_print(const Stats *s, FILE *out, int top);

#endif
