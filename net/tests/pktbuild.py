"""Build packets by hand (no third-party libraries): Ethernet, IPv4, IPv6, TCP, UDP.

Used by the reassembly tests, which need exact control over fragments and sequence numbers.
Checksums are computed properly so --check stays quiet on them.
"""

import socket
import struct

ME, PEER = "192.0.2.10", "198.51.100.20"
ME6, PEER6 = "2001:db8::10", "2001:db8::20"
MAC_A, MAC_B = bytes.fromhex("02aa00000010"), bytes.fromhex("02bb00000020")


def csum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack(f">{len(data) // 2}H", data))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return ~s & 0xFFFF


def eth(payload, ethertype=0x0800, src=MAC_A, dst=MAC_B):
    return dst + src + struct.pack(">H", ethertype) + payload


def pseudo4(src, dst, proto, length):
    return socket.inet_aton(src) + socket.inet_aton(dst) + struct.pack(">BBH", 0, proto, length)


def pseudo6(src, dst, proto, length):
    return socket.inet_pton(socket.AF_INET6, src) + socket.inet_pton(socket.AF_INET6, dst) + struct.pack(">IxxxB", length, proto)


def tcp_segment(src, dst, sport, dport, seq, ack, flags, payload=b"", window=64240, v6=False):
    hdr = struct.pack(">HHIIBBHHH", sport, dport, seq & 0xFFFFFFFF, ack & 0xFFFFFFFF, 5 << 4, flags, window, 0, 0)
    ps = (pseudo6 if v6 else pseudo4)(src, dst, 6, len(hdr) + len(payload))
    c = csum(ps + hdr + payload)
    return hdr[:16] + struct.pack(">H", c) + hdr[18:] + payload


def udp_datagram(src, dst, sport, dport, payload, v6=False):
    length = 8 + len(payload)
    hdr = struct.pack(">HHHH", sport, dport, length, 0)
    c = csum((pseudo6 if v6 else pseudo4)(src, dst, 17, length) + hdr + payload) or 0xFFFF
    return struct.pack(">HHHH", sport, dport, length, c) + payload


def ipv4(src, dst, proto, payload, ident=1, more=False, frag_offset=0, ttl=64):
    """frag_offset is in bytes (a multiple of 8)."""
    assert frag_offset % 8 == 0
    flags_frag = (0x2000 if more else 0) | (frag_offset // 8)
    hdr = struct.pack(">BBHHHBBH4s4s", 0x45, 0, 20 + len(payload), ident, flags_frag, ttl, proto, 0,
                      socket.inet_aton(src), socket.inet_aton(dst))
    return hdr[:10] + struct.pack(">H", csum(hdr)) + hdr[12:] + payload


def ipv6(src, dst, next_header, payload, hop=64):
    return (struct.pack(">IHBB", 6 << 28, len(payload), next_header, hop) + socket.inet_pton(socket.AF_INET6, src) +
            socket.inet_pton(socket.AF_INET6, dst) + payload)


def ipv6_fragment(src, dst, proto, data, ident, frag_offset, more):
    """One fragment: `data` is this piece of the upper-layer message."""
    assert frag_offset % 8 == 0
    fh = struct.pack(">BBHI", proto, 0, ((frag_offset // 8) << 3) | (1 if more else 0), ident)
    return ipv6(src, dst, 44, fh + data)


def fragment_v4(src, dst, proto, payload, ident, size=1480, order=None):
    """Split a payload into IPv4 fragments of `size` bytes (a multiple of 8). `order` reorders them."""
    pieces = []
    for off in range(0, len(payload), size):
        chunk = payload[off:off + size]
        pieces.append(ipv4(src, dst, proto, chunk, ident=ident, more=off + size < len(payload), frag_offset=off))
    if order is not None:
        pieces = [pieces[i] for i in order]
    return pieces


def fragment_v6(src, dst, proto, payload, ident, size=1232, order=None):
    pieces = []
    for off in range(0, len(payload), size):
        pieces.append(ipv6_fragment(src, dst, proto, payload[off:off + size], ident, off, off + size < len(payload)))
    if order is not None:
        pieces = [pieces[i] for i in order]
    return pieces


SYN, FIN, RST, PSH, ACK = 0x02, 0x01, 0x04, 0x08, 0x10


class Conversation:
    """A TCP connection built segment by segment; each method returns Ethernet frames."""

    def __init__(self, client=ME, server=PEER, cport=40000, sport=80, isn_c=1000, isn_s=9000, v6=False):
        self.c, self.s, self.cport, self.sport = client, server, cport, sport
        self.isn_c, self.isn_s, self.v6 = isn_c, isn_s, v6

    def _frame(self, src, dst, sport, dport, seq, ack, flags, payload=b""):
        seg = tcp_segment(src, dst, sport, dport, seq, ack, flags, payload, v6=self.v6)
        if self.v6:
            return eth(ipv6(src, dst, 6, seg), 0x86DD)
        return eth(ipv4(src, dst, 6, seg))

    def c2s(self, seq_off, payload=b"", flags=ACK, ack_off=1):
        """A client segment whose first byte is `seq_off` bytes into the client's data."""
        return self._frame(self.c, self.s, self.cport, self.sport, self.isn_c + 1 + seq_off, self.isn_s + ack_off, flags, payload)

    def s2c(self, seq_off, payload=b"", flags=ACK, ack_off=1):
        return self._frame(self.s, self.c, self.sport, self.cport, self.isn_s + 1 + seq_off, self.isn_c + ack_off, flags, payload)

    def handshake(self):
        return [
            self._frame(self.c, self.s, self.cport, self.sport, self.isn_c, 0, SYN),
            self._frame(self.s, self.c, self.sport, self.cport, self.isn_s, self.isn_c + 1, SYN | ACK),
            self.c2s(0),
        ]

    def close(self, c_len, s_len):
        return [
            self.c2s(c_len, flags=FIN | ACK, ack_off=1 + s_len),
            self.s2c(s_len, flags=FIN | ACK, ack_off=2 + c_len),
            self.c2s(c_len + 1, flags=ACK, ack_off=2 + s_len),
        ]
