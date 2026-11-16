# pktscope

A packet capture analyzer in C, in the spirit of `tcpdump` and a small piece of Wireshark. It reads `.pcap` and `.pcapng` files (or captures live on Linux), decodes the packets layer by layer, filters them, and summarizes who talked to whom.

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

`examples/demo.pcap` is an invented browsing session (ARP, a DNS lookup, an HTTP fetch, a ping) using addresses reserved for documentation. `examples/make_demo.py` rebuilds it.

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
- `src/live.c` is the Linux capture path.

## Tests

```
make test       # unit tests, then end-to-end and cross-check tests
make sanitize   # the same, built with AddressSanitizer and UBSan
```

- **Unit tests** (`tests/test_unit.c`, 930+ checks): every decoder against packets built by scapy (`tests/gen_vectors.py` writes them into `tests/vectors.h`), the filter language, the pcap and pcapng readers, and about 100,000 mutated and random packets to shake out crashes.
- **End-to-end tests** (`tests/test_cli.py`): run the real program on generated captures in every supported file format, check each option, bad input, and a live loopback capture of real traffic.
- **Cross-check** (`tests/test_crosscheck.py`): 1,500 random packets each for TCP, UDP and IPv6 mixes, built by scapy, compared field by field with pktscope's JSON output, including whether the checksums are good. Deliberately breaking the pseudo-header calculation makes this test fail. It needs scapy; set `REQUIRE_REFERENCE=1` to fail rather than skip when it is missing (CI does).

## Limitations

- No TCP stream reassembly: HTTP is recognised only when the request or status line is at the start of one packet.
- No IP fragment reassembly. Fragments are decoded and labelled, but later fragments have no transport header to show.
- Live capture is Linux only and handles Ethernet and loopback interfaces.
- DNS responses show at most four answers.
- Checksums on packets captured on the machine that sent them often look wrong, because the network card fills them in after the capture point (checksum offload). That is why bad checksums are hidden by default; use `--check` for captures taken elsewhere.
- IPv4 and IPv6 only; other network protocols are shown as unknown ethertypes.
