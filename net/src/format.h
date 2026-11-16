// Turning a decoded Packet into text: a tcpdump-style line, or one JSON object.
#ifndef FORMAT_H
#define FORMAT_H

#include <stdio.h>

#include "decode.h"

// "14:03:22.123456 IP 10.0.0.5.51514 > 93.184.216.34.80: TCP [S] seq 100 win 64240 len 0".
// With epoch != 0 the time is seconds since 1970 instead of a time of day (UTC).
// With show_csum != 0, packets whose checksums don't verify get a "[bad ... checksum]" tag.
void format_line(const Packet *p, int epoch, int show_csum, char *out, size_t n);
// One JSON object on one line (newline-delimited JSON), without a trailing newline.
void format_json(const Packet *p, FILE *out);
// A hex + ASCII dump of up to `max` payload bytes, 16 per row, each row indented.
void format_hex(const uint8_t *data, size_t len, size_t max, FILE *out);

#endif
