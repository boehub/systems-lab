#!/usr/bin/env python3
"""Build examples/demo.pcap: a short, invented browsing session (needs scapy).

    python3 examples/make_demo.py

A laptop (192.0.2.10) finds its router with ARP, looks up www.example.test, opens a
TCP connection to the web server, fetches a page, closes the connection, then pings the
router. Every address is from the ranges reserved for documentation.
"""

import logging
import os
import sys

logging.getLogger("scapy.runtime").setLevel(logging.ERROR)
from scapy.all import ARP, DNS, DNSQR, DNSRR, ICMP, IP, TCP, UDP, Ether, Raw  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests"))
from capwriter import write_pcap  # noqa: E402

ME, ROUTER, WEB, DNS_SRV = "192.0.2.10", "192.0.2.1", "198.51.100.80", "192.0.2.53"
M_ME, M_RT = "02:aa:00:00:00:10", "02:bb:00:00:00:01"

out_eth, in_eth = Ether(src=M_ME, dst=M_RT), Ether(src=M_RT, dst=M_ME)
page = b"<html><body>Hello from the demo server</body></html>"
request = b"GET /index.html HTTP/1.1\r\nHost: www.example.test\r\nUser-Agent: demo\r\n\r\n"
response = (b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n" % len(page)) + page

cport, isn_c, isn_s = 49152, 1000, 7000
frames = [
    Ether(src=M_ME, dst="ff:ff:ff:ff:ff:ff") / ARP(op=1, hwsrc=M_ME, psrc=ME, pdst=ROUTER),
    Ether(src=M_RT, dst=M_ME) / ARP(op=2, hwsrc=M_RT, psrc=ROUTER, hwdst=M_ME, pdst=ME),
    out_eth / IP(src=ME, dst=DNS_SRV) / UDP(sport=53311, dport=53) / DNS(id=0x2B2B, rd=1, qd=DNSQR(qname="www.example.test", qtype="A")),
    in_eth / IP(src=DNS_SRV, dst=ME) / UDP(sport=53, dport=53311) / DNS(
        id=0x2B2B, qr=1, rd=1, ra=1, ancount=1, qd=DNSQR(qname="www.example.test", qtype="A"),
        an=DNSRR(rrname="www.example.test", type="A", ttl=300, rdata=WEB)),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="S", seq=isn_c, window=64240),
    in_eth / IP(src=WEB, dst=ME) / TCP(sport=80, dport=cport, flags="SA", seq=isn_s, ack=isn_c + 1, window=65535),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="A", seq=isn_c + 1, ack=isn_s + 1, window=64240),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="PA", seq=isn_c + 1, ack=isn_s + 1, window=64240) / Raw(request),
    in_eth / IP(src=WEB, dst=ME) / TCP(sport=80, dport=cport, flags="A", seq=isn_s + 1, ack=isn_c + 1 + len(request), window=65535),
    in_eth / IP(src=WEB, dst=ME) / TCP(sport=80, dport=cport, flags="PA", seq=isn_s + 1, ack=isn_c + 1 + len(request), window=65535) / Raw(response),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="A", seq=isn_c + 1 + len(request), ack=isn_s + 1 + len(response), window=64240),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="FA", seq=isn_c + 1 + len(request), ack=isn_s + 1 + len(response), window=64240),
    in_eth / IP(src=WEB, dst=ME) / TCP(sport=80, dport=cport, flags="FA", seq=isn_s + 1 + len(response), ack=isn_c + 2 + len(request), window=65535),
    out_eth / IP(src=ME, dst=WEB) / TCP(sport=cport, dport=80, flags="A", seq=isn_c + 2 + len(request), ack=isn_s + 2 + len(response), window=64240),
    out_eth / IP(src=ME, dst=ROUTER) / ICMP(type=8, id=1, seq=1) / Raw(b"abcdefghijklmnop"),
    in_eth / IP(src=ROUTER, dst=ME) / ICMP(type=0, id=1, seq=1) / Raw(b"abcdefghijklmnop"),
]
gaps = [0, 0.0008, 0.0502, 0.0124, 0.0003, 0.0231, 0.0002, 0.0004, 0.0215, 0.0018, 0.0002, 0.0301, 0.0210, 0.0002, 0.9000, 0.0009]
t, packets = 1700000000.0, []
for gap, f in zip(gaps, frames):
    t += gap
    packets.append((t, bytes(f)))
write_pcap(os.path.join(HERE, "demo.pcap"), packets)
print(f"wrote demo.pcap ({len(packets)} packets)")
