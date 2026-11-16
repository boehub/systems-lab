# pktscope

A packet capture analyzer in C, in the spirit of `tcpdump` and a small piece of Wireshark. It reads `.pcap` and `.pcapng` files (or captures live on Linux), decodes the packets layer by layer, filters them, puts IP fragments and TCP streams back together, and summarizes who talked to whom and what was said.

No dependencies beyond a C compiler and the standard library.

```
$ make pktscope
$ ./pktscope examples/demo.pcap
22:13:20.000000 ARP who-has 192.0.2.1 tell 192.0.2.10
22:13:20.051000 IP 192.0.2.10.53311 > 192.0.2.53.53: UDP len 34 | DNS query id 11051 A www.example.test
22:13:20.063400 IP 192.0.2.53.53 > 192.0.2.10.53311: UDP len 66 | DNS response id 11051 A www.example.test rcode 0 => A 198.51.100.80
22:13:20.063700 IP 192.0.2.10.49152 > 198.51.100.80.80: TCP [S] seq 1000 win 64240 len 0
22:13:20.087400 IP 192.0.2.10.49152 > 198.51.100.80.80: TCP [P.] seq 1001 ack 7001 win 64240 len 70 | HTTP GET /index.html HTTP/1.1
...
```

`examples/demo.pcap` is an invented browsing session (ARP, a DNS lookup, an HTTP fetch, a ping) using addresses reserved for documentation. `examples/make_demo.py` rebuilds it (it needs scapy). `examples/messy.pcap` is a harder one: downloads whose segments arrive out of order, are repeated or are lost, and a UDP datagram in three fragments that arrive in the wrong order. `examples/make_messy.py` rebuilds that with plain Python.

## Usage

```
pktscope [options] FILE          (FILE of - reads standard input)
pktscope --live IFACE [options]

  -f EXPR       only show packets matching a filter
  -c N          stop after N packets
  -s, --summary print totals, top conversations, DNS names and HTTP requests
  --json        one JSON object per packet (newline-delimited)
  -x            hex dump of each packet
  --check       verify checksums and flag the bad ones
  --epoch       print timestamps as seconds since 1970
  --top N       conversations to list in the summary (default 10)
  -w FILE       also save the matching packets to a pcap file (this converts pcapng to pcap)
  -q, --quiet   with -w: don't print the packets
  --streams     list the TCP connections: endpoints, state, bytes each way, loss and reordering
  --follow N    print what connection N said, in order (-x for hex; --raw client|server for one side's exact bytes)
  --http        list the HTTP requests and responses found inside the TCP connections
  --no-defrag   do not put IP fragments back together
```

### Summary

```
$ ./pktscope examples/demo.pcap -s
16 packets, 1110 bytes, 1.063 seconds
  TCP                10
  UDP                2
  ICMP               2
  not IP (ARP, ...)  2

Busiest conversations (3 in total):
  proto endpoints                                       packets     bytes  state
  TCP   192.0.2.10.49152 <-> 198.51.100.80.80                10       726  closed
  ...
```

### Filters

```
./pktscope cap.pcap -f 'tcp and port 80 and syn'
./pktscope cap.pcap -f 'host 192.0.2.10 and not arp'
./pktscope cap.pcap -f 'net 198.51.100.0/24 and greater 1000'
./pktscope cap.pcap -f '(dns or http) and src host 192.0.2.10'
```

Words: `tcp udp icmp icmp6 ip ip6 arp dns http syn fin rst vlan badcsum`. Forms: `host`, `net`, `port` (each optionally prefixed by `src` or `dst`), `greater N`, `less N`. Combine with `and`/`or`/`not` (or `&&`, `||`, `!`) and parentheses.

### TCP streams

A packet listing shows segments one at a time, so a web page split over twenty segments is twenty lines. `--streams`, `--follow` and `--http` join the segments of each connection back into the two byte streams, whatever order they arrived in.

```
$ ./pktscope examples/demo.pcap --streams
TCP connections (1):
   id  client                    server                    state       client->   ->client  notes
    0  192.0.2.10.49152          198.51.100.80.80          closed            70        116

$ ./pktscope examples/demo.pcap --http
HTTP requests and responses:
  stream 0  GET /index.html  [www.example.test]  ->  200 OK  text/html  52 bytes

$ ./pktscope examples/demo.pcap --follow 0
Stream 0: 192.0.2.10.49152 <-> 198.51.100.80.80 (closed)

--- client -> server (70 bytes) ---
GET /index.html HTTP/1.1
Host: www.example.test
...

$ ./pktscope examples/messy.pcap --streams
TCP connections (2):
   id  client                    server                    state       client->   ->client  notes
    0  192.0.2.10.50001          198.51.100.20.80          closed            51       1163  1 retransmitted, 1 out of order
    1  192.0.2.10.50002          198.51.100.20.80          closed            51        863  1 out of order, 1 gap, 300 bytes missing

$ ./pktscope examples/messy.pcap --follow 0 --raw server > response.bin     # the bytes, exactly as sent
```

Retransmissions are dropped, reordered segments are put in place, and bytes the capture never saw are shown as `[N bytes missing from the capture]` rather than silently skipped. The notes column says what was seen (`2 retransmitted`, `1 out of order`, `1 gap, 100 bytes missing`). A new SYN on ports that carried an earlier connection starts a new stream, and a capture that begins mid-connection is followed from the first data it sees. The `-f` filter decides which packets are tracked.

### IP fragments

When a datagram was too big for the link and was sent in pieces, only the first piece has the transport header. pktscope collects the pieces, in any order, and when the last one arrives prints the whole datagram as an extra line:

```
22:13:20.028000 [reassembled from 3 fragments] IP 192.0.2.10.41000 > 198.51.100.20.9999: UDP len 3072
```

The reassembled datagram is decoded like any other, so its checksum is verified, DNS split across fragments is read, and a TCP segment sent in fragments joins its stream. Filters see the datagram, not the pieces (`-f 'dns'` finds a DNS answer that arrived in two fragments), but the pieces are collected whether or not they match.

### Saving and piping

```
./pktscope big.pcapng -f 'host 192.0.2.10 and port 53' -w dns.pcap -q   # cut out a piece
zcat capture.pcap.gz | ./pktscope - -s                                      # read from a pipe
```

### Live capture

```
sudo ./pktscope --live lo -f 'tcp or udp' -c 20
```

Uses an `AF_PACKET` socket, so it needs Linux and root. On loopback each packet is shown once.

## What it decodes

- **Files:** pcap in either byte order with microsecond or nanosecond timestamps; pcapng (section, interface, enhanced and simple packet blocks, per-interface timestamp resolution).
- **Link layers:** Ethernet (with 802.1Q VLAN tags), raw IP, Linux cooked (SLL), BSD loopback.
- **Network:** ARP, IPv4 (options, fragments), IPv6 (extension headers, fragments).
- **Transport:** TCP (flags, sequence numbers, window), UDP, ICMP, ICMPv6.
- **Application:** DNS (names with compression pointers, with loop guards; first four answers shown) and HTTP request and status lines.
- **Checksums:** IPv4 header, TCP, UDP, ICMP and ICMPv6, including the pseudo-header. Bad ones are hidden unless you pass `--check` (see below).

## How it works

- `src/pcap.c` reads capture files into packets and also writes pcap files.
- `src/decode.c` parses one packet into a `Packet` struct, bounds-checking every read, so a truncated or hostile packet gives a partial decode and never an out-of-range access.
- `src/filter.c` compiles a filter string into a small expression tree and evaluates it per packet.
- `src/format.c` prints a packet as text, JSON or hex.
- `src/flows.c` tracks conversations (a hash table keyed by protocol and both endpoints, in either direction), TCP state from the flags, DNS names and HTTP requests.
- `src/defrag.c` collects IP fragments per datagram (keyed by source, destination, protocol and identification), tracks which bytes have arrived, and when they cover the whole length builds a complete IP packet to decode again.
- `src/streams.c` follows each TCP connection. Every direction keeps the next sequence number it expects (compared with wrap-around arithmetic, so 32-bit overflow is fine). A segment that starts there is appended; one from the past is trimmed or counted as a repeat; one from the future is held in a sorted list until the bytes before it arrive, and at the end of the capture whatever is still held is joined with a marked gap. The same file reads HTTP out of the finished streams: request and status lines, `Content-Length`, chunked bodies, no-body responses to `HEAD`, and pairs each response with its request.
- `src/live.c` is the Linux capture path.

## Tests

```
make test       # unit tests, then end-to-end, reassembly and cross-check tests
make sanitize   # the same, built with AddressSanitizer and UBSan
```

- **Unit tests** (`tests/test_unit.c`, 930+ checks): every decoder against packets built by scapy (`tests/gen_vectors.py` writes them into `tests/vectors.h`), the filter language, the pcap and pcapng readers, and about 100,000 mutated and random packets to shake out crashes.
- **End-to-end tests** (`tests/test_cli.py`): run the real program on generated captures in every supported file format, check each option, bad input, and a live loopback capture of real traffic.
- **Reassembly tests** (`tests/test_reassembly.py`, 53 tests): packets are built by hand (`tests/pktbuild.py`, no libraries) so each case says exactly what was sent. Fragments in all six arrival orders, duplicated, interleaved, missing, from different senders, over IPv6; TCP segments reordered, repeated, overlapping, lost, wrapping past 2^32, captured from the middle, reused ports; HTTP split across segments, pipelined, chunked, `HEAD`, truncated. One test sends 25 random conversations (random sizes, shuffled, with repeats) and requires every byte back exactly; another damages 250 captures at random and requires no crash. Making the reassembler's overlap rule wrong makes these fail.
- **Cross-check** (`tests/test_crosscheck.py`): 1,500 random packets each for TCP, UDP and IPv6 mixes, built by scapy, compared field by field with pktscope's JSON output, including whether the checksums are good. Deliberately breaking the pseudo-header calculation makes this test fail. It needs scapy; set `REQUIRE_REFERENCE=1` to fail rather than skip when it is missing (CI does).

## Limitations

- The per-packet lines still look at one packet at a time: HTTP is recognised there only when the request or status line starts a packet. `--http` and `--follow` use the reassembled streams and have no such limit.
- Stream reassembly covers TCP only, and only when you ask for `--streams`, `--follow` or `--http`. It does not decrypt TLS, decode HTTP/2, or decompress `gzip` bodies; a body is counted, not shown. Up to 256 messages per connection are paired, in order (so HTTP pipelining works but a server answering out of order would be mismatched). Each direction keeps at most 32 MB, and a segment more than 1 MB ahead of the expected byte is ignored.
- Where TCP segments overlap with different data, the first copy wins. The same goes for overlapping IP fragments.
- IP fragments: the pieces of a datagram are kept until the end of the capture (there is no timeout), at most 256 datagrams at once, the oldest dropped first. IPv4 header options and IPv6 extension headers that come before the fragment header are not carried into the rebuilt datagram.
- Live capture is Linux only and handles Ethernet and loopback interfaces.
- DNS responses show at most four answers.
- Checksums on packets captured on the machine that sent them often look wrong, because the network card fills them in after the capture point (checksum offload). That is why bad checksums are hidden by default; use `--check` for captures taken elsewhere.
- IPv4 and IPv6 only; other network protocols are shown as unknown ethertypes.
