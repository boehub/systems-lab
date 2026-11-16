#!/usr/bin/env python3
"""Build examples/messy.pcap: a capture that is hard to read packet by packet.

    python3 examples/make_messy.py          (needs only Python)

  * a web page downloaded over TCP, whose segments arrive out of order, with one repeated
  * a large UDP datagram sent in three IP fragments, which arrive in the wrong order
  * a second download that loses one segment before the capture could see it

Try:  pktscope examples/messy.pcap --streams
      pktscope examples/messy.pcap --http
      pktscope examples/messy.pcap --follow 0
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "tests"))
import pktbuild as pb  # noqa: E402
from capwriter import write_pcap  # noqa: E402

page = b"<html><body>" + b"".join(b"<p>line %d of the page</p>\n" % i for i in range(1, 41)) + b"</body></html>"
response = b"HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\n\r\n" % len(page) + page
request = b"GET /page.html HTTP/1.1\r\nHost: www.example.test\r\n\r\n"

frames = []

# download 1: segments of 300 bytes, reordered, one repeated
c = pb.Conversation(cport=50001, isn_c=4000, isn_s=90000)
segs = [c.s2c(o, response[o:o + 300], flags=pb.ACK | pb.PSH, ack_off=1 + len(request)) for o in range(0, len(response), 300)]
segs[1], segs[2] = segs[2], segs[1]
segs.insert(4, segs[3])
frames += c.handshake() + [c.c2s(0, request, flags=pb.ACK | pb.PSH)] + segs + c.close(len(request), len(response))

# a UDP datagram split into three fragments that arrive as 3rd, 1st, 2nd
dgram = pb.udp_datagram(pb.ME, pb.PEER, 41000, 9999, bytes(range(256)) * 12)
frames += [pb.eth(f) for f in pb.fragment_v4(pb.ME, pb.PEER, 17, dgram, 77, order=[2, 0, 1])]

# download 2: the third segment never reached the capture
d = pb.Conversation(cport=50002, isn_c=100, isn_s=700)
segs = [d.s2c(o, response[o:o + 300], flags=pb.ACK | pb.PSH, ack_off=1 + len(request)) for o in range(0, len(response), 300)]
del segs[2]
frames += d.handshake() + [d.c2s(0, request, flags=pb.ACK | pb.PSH)] + segs + d.close(len(request), len(response))

write_pcap(os.path.join(HERE, "messy.pcap"), [(1700000000.0 + i * 0.002, f) for i, f in enumerate(frames)])
print(f"wrote messy.pcap ({len(frames)} packets)")
