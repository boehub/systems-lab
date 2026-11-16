// Following TCP connections: putting each direction's segments back in order, so that what
// the two sides actually said can be read, even when packets were reordered, repeated or lost.
//
//   streams_add()    feed every packet (non-TCP packets are ignored)
//   streams_finish() once, after the last packet
//   then list the streams, follow one, or read the HTTP in all of them.
#ifndef STREAMS_H
#define STREAMS_H

#include <stdio.h>

#include "decode.h"

typedef struct StreamTable StreamTable;

typedef enum {
    FOLLOW_TEXT,        // both directions, in the order the data arrived, printable text
    FOLLOW_HEX,         // the same as a hex dump
    FOLLOW_RAW_CLIENT,  // only the client's bytes, exactly as sent
    FOLLOW_RAW_SERVER   // only the server's bytes
} FollowMode;

StreamTable *streams_new(void);
void streams_free(StreamTable *t);
void streams_add(StreamTable *t, const Packet *p);
void streams_finish(StreamTable *t);

size_t streams_count(const StreamTable *t);

// One line per connection: endpoints, state, bytes each way, and anything odd that was seen.
void streams_list(const StreamTable *t, FILE *out);

// Print stream `id` (as numbered by streams_list). Returns -1 when there is no such stream.
int streams_follow(const StreamTable *t, size_t id, FollowMode mode, FILE *out);

// Find the HTTP requests and responses inside the streams and pair them up.
void streams_http(const StreamTable *t, FILE *out);

// The reassembled bytes of one direction (0 client to server, 1 server to client).
const uint8_t *stream_data(const StreamTable *t, size_t id, int dir, size_t *len);
// Bytes known to be missing from that direction (packets the capture never saw).
uint64_t stream_missing(const StreamTable *t, size_t id, int dir);

#endif
