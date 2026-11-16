"""IP fragment reassembly and TCP stream reassembly, driven through the real program.

The packets are built by hand (pktbuild.py), so the tests do not need scapy, and each case
states exactly what was sent: fragments in odd orders, segments repeated, reordered, overlapping
or lost.
"""

import json
import os
import random
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
BIN = os.path.join(os.path.dirname(HERE), "pktscope")

import pktbuild as pb  # noqa: E402
from capwriter import write_pcap  # noqa: E402


def run(*args, expect=0):
    r = subprocess.run([BIN, *args], capture_output=True, timeout=60)
    assert r.returncode == expect, f"exit {r.returncode}, wanted {expect}\n{r.stdout[:500]!r}\n{r.stderr!r}"
    return r


def text(*args, expect=0):
    r = run(*args, expect=expect)
    return r.stdout.decode(errors="replace"), r.stderr.decode(errors="replace")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cap = os.path.join(self.tmp.name, "t.pcap")

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, frames, linktype=1):
        write_pcap(self.cap, [(1000.0 + i * 0.01, f) for i, f in enumerate(frames)], linktype=linktype)
        return self.cap

    def lines(self, *args):
        return text(self.cap, *args)[0].splitlines()


# ---- IP fragments ---------------------------------------------------------------------------

class TestFragments(Base):
    PAYLOAD = bytes(range(256)) * 12  # 3072 bytes

    def udp(self):
        return pb.udp_datagram(pb.ME, pb.PEER, 5000, 6000, self.PAYLOAD)

    def frames(self, order=None, ident=7):
        return [pb.eth(f) for f in pb.fragment_v4(pb.ME, pb.PEER, 17, self.udp(), ident, order=order)]

    def reassembled(self):
        return [l for l in self.lines() if "reassembled" in l]

    def test_in_order(self):
        self.write(self.frames())
        lines = self.lines()
        self.assertEqual(len(lines), 4)  # three pieces and the datagram
        self.assertEqual(sum("fragment, offset" in l for l in lines), 2)  # the first piece reads like a normal packet
        last = lines[-1]
        self.assertIn("[reassembled from 3 fragments]", last)
        self.assertIn("UDP len 3072", last)
        self.assertIn("192.0.2.10.5000 > 198.51.100.20.6000", last)

    def test_every_arrival_order(self):
        import itertools
        for order in itertools.permutations(range(3)):
            with self.subTest(order=order):
                self.write(self.frames(order=list(order)))
                r = self.reassembled()
                self.assertEqual(len(r), 1, order)
                self.assertIn("UDP len 3072", r[0])

    def test_reassembled_datagram_has_good_checksums(self):
        self.write(self.frames(order=[2, 0, 1]))
        self.assertEqual(self.lines("-f", "badcsum", "--check"), [])
        # and a corrupted payload byte is caught: the checksum covers the whole datagram
        frames = self.frames()
        bad = bytearray(frames[1])
        bad[60] ^= 0xFF
        frames[1] = bytes(bad)
        self.write(frames)
        self.assertEqual(len(self.lines("-f", "badcsum", "--check")), 1)

    def test_missing_piece_never_completes(self):
        self.write(self.frames()[:1] + self.frames()[2:])
        self.assertEqual(self.reassembled(), [])
        _, err = text(self.cap)
        self.assertIn("never completed", err)

    def test_duplicate_pieces(self):
        f = self.frames()
        self.write([f[0], f[1], f[1], f[0], f[2], f[2]])
        self.assertEqual(len(self.reassembled()), 1)

    def test_two_datagrams_interleaved(self):
        a, b = self.frames(ident=1), self.frames(ident=2)
        self.write([a[0], b[0], b[1], a[1], a[2], b[2]])
        self.assertEqual(len(self.reassembled()), 2)

    def test_different_senders_do_not_mix(self):
        a = [pb.eth(f) for f in pb.fragment_v4("192.0.2.1", pb.PEER, 17, self.udp(), 5)]
        b = [pb.eth(f) for f in pb.fragment_v4("192.0.2.2", pb.PEER, 17, self.udp(), 5)]
        self.write([a[0], b[1], a[1], b[0], a[2], b[2]])
        self.assertEqual(len(self.reassembled()), 2)

    def test_filter_applies_to_the_whole_datagram(self):
        self.write(self.frames())
        lines = self.lines("-f", "udp and port 6000")
        self.assertEqual(len(lines), 2)  # the first piece (it has the UDP header) and the datagram
        self.assertTrue(lines[-1].count("reassembled"))
        self.assertEqual(len(self.lines("-f", "dns")), 0)

    def test_no_defrag(self):
        self.write(self.frames())
        lines = self.lines("--no-defrag")
        self.assertEqual(len(lines), 3)
        self.assertFalse(any("reassembled" in l for l in lines))

    def test_json_says_where_it_came_from(self):
        self.write(self.frames())
        objs = [json.loads(l) for l in self.lines("--json")]
        self.assertEqual([o.get("reassembled_from") for o in objs], [None, None, None, 3])
        self.assertEqual(objs[-1]["udp"]["dport"] if "udp" in objs[-1] else objs[-1].get("dport"), 6000)

    def test_summary_does_not_count_pieces_twice(self):
        self.write(self.frames())
        out = text(self.cap, "-s")[0]
        self.assertIn("3 packets", out)
        self.assertIn("UDP", out)

    def test_ipv4_fragment_with_ethernet_padding(self):
        frames = [f + b"\0" * 6 for f in self.frames()]
        self.write(frames)
        r = self.reassembled()
        self.assertEqual(len(r), 1)
        self.assertIn("UDP len 3072", r[0])

    def test_ipv6(self):
        payload = pb.udp_datagram(pb.ME6, pb.PEER6, 5000, 6000, self.PAYLOAD, v6=True)
        pieces = pb.fragment_v6(pb.ME6, pb.PEER6, 17, payload, 0x1234, order=[1, 2, 0])
        self.write([pb.eth(p, 0x86DD) for p in pieces])
        r = self.reassembled()
        self.assertEqual(len(r), 1)
        self.assertIn("[reassembled from 3 fragments]", r[0])
        self.assertIn("UDP len 3072", r[0])
        self.assertEqual(self.lines("-f", "badcsum", "--check"), [])

    def test_dns_response_split_across_fragments_is_decoded(self):
        # a big DNS answer arrives in two pieces; only the whole datagram can be read as DNS
        name = b"".join(bytes([len(p)]) + p.encode() for p in "big.example.test".split(".")) + b"\0"
        question = name + b"\x00\x01\x00\x01"
        answers = b"".join(b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x01\x2c\x00\x04" + bytes([198, 51, 100, i]) for i in range(1, 120))
        dns = b"\xab\xcd\x81\x80\x00\x01" + (119).to_bytes(2, "big") + b"\0\0\0\0" + question + answers
        self.assertGreater(len(dns), 1480)
        dg = pb.udp_datagram(pb.PEER, pb.ME, 53, 41000, dns)
        self.write([pb.eth(f) for f in pb.fragment_v4(pb.PEER, pb.ME, 17, dg, 9, order=[1, 0])])
        r = [l for l in self.lines() if "reassembled" in l]
        self.assertEqual(len(r), 1)
        self.assertIn("DNS response", r[0])
        self.assertIn("big.example.test", r[0])
        self.assertIn("(+115 more)", r[0])  # 119 answers, the first four are shown
        self.assertNotIn("malformed", r[0])

    def test_tcp_segment_split_into_fragments_joins_the_stream(self):
        c = pb.Conversation()
        body = bytes(range(256)) * 10
        seg = pb.tcp_segment(c.s, c.c, c.sport, c.cport, c.isn_s + 1, c.isn_c + 1, pb.ACK | pb.PSH, body)
        frames = c.handshake() + [pb.eth(f) for f in pb.fragment_v4(c.s, c.c, 6, seg, 3, size=1000)]
        self.write(frames)
        out = text(self.cap, "--streams")[0]
        self.assertIn(str(len(body)), out)
        raw = run(self.cap, "--follow", "0", "--raw", "server").stdout
        self.assertEqual(raw, body)

    def test_unreasonable_fragment_is_ignored(self):
        # a fragment claiming to extend beyond the largest possible datagram
        frame = pb.eth(pb.ipv4(pb.ME, pb.PEER, 17, b"x" * 64, ident=1, more=False, frag_offset=65528))
        self.write([frame])
        text(self.cap)  # must not crash


# ---- TCP streams --------------------------------------------------------------------------

def stream_cap(case, c2s=b"", s2c=b"", **kw):
    return case.write(case_frames(c2s, s2c, **kw))


def case_frames(c2s, s2c, seg=100, order=None, dup=(), drop=(), conv=None, close=True):
    """A connection that carries c2s then s2c, cut into `seg`-byte segments."""
    c = conv or pb.Conversation()
    cs = [c.c2s(o, c2s[o:o + seg], flags=pb.ACK | pb.PSH) for o in range(0, len(c2s), seg)]
    ss = [c.s2c(o, s2c[o:o + seg], flags=pb.ACK | pb.PSH, ack_off=1 + len(c2s)) for o in range(0, len(s2c), seg)]
    segs = cs + ss
    if order is not None:
        segs = [segs[i] for i in order]
    for i in dup:
        segs.insert(i + 1, segs[i])
    segs = [s for i, s in enumerate(segs) if i not in drop]
    return c.handshake() + segs + (c.close(len(c2s), len(s2c)) if close else [])


class TestStreams(Base):
    A = bytes(random.Random(1).randrange(256) for _ in range(950))
    B = bytes(random.Random(2).randrange(256) for _ in range(1234))

    def follow(self, n=0, side="server"):
        return run(self.cap, "--follow", str(n), "--raw", side).stdout

    def check_data(self):
        self.assertEqual(self.follow(0, "client"), self.A)
        self.assertEqual(self.follow(0, "server"), self.B)

    def test_in_order(self):
        stream_cap(self, self.A, self.B)
        self.check_data()
        out = text(self.cap, "--streams")[0]
        self.assertIn("closed", out)
        self.assertIn(str(len(self.A)), out)
        self.assertIn(str(len(self.B)), out)

    def test_out_of_order(self):
        n = (len(self.A) + 99) // 100 + (len(self.B) + 99) // 100
        order = list(range(n))
        random.Random(5).shuffle(order)
        stream_cap(self, self.A, self.B, order=order)
        self.check_data()
        self.assertIn("out of order", text(self.cap, "--streams")[0])

    def test_reversed(self):
        n = (len(self.A) + 99) // 100 + (len(self.B) + 99) // 100
        stream_cap(self, self.A, self.B, order=list(range(n))[::-1])
        self.check_data()

    def test_retransmissions_are_dropped(self):
        stream_cap(self, self.A, self.B, dup=(0, 3, 9, 12))
        self.check_data()
        self.assertIn("retransmitted", text(self.cap, "--streams")[0])

    def test_retransmission_cut_differently(self):
        c = pb.Conversation()
        frames = c.handshake()
        frames.append(c.c2s(0, self.A[:300], flags=pb.ACK | pb.PSH))
        frames.append(c.c2s(100, self.A[100:550], flags=pb.ACK | pb.PSH))   # overlaps what was sent, and extends it
        frames.append(c.c2s(500, self.A[500:], flags=pb.ACK | pb.PSH))      # overlaps again
        frames += c.close(len(self.A), 0)
        self.write(frames)
        self.assertEqual(self.follow(0, "client"), self.A)

    def test_lost_segment_leaves_a_marked_gap(self):
        stream_cap(self, self.A, b"", drop=(3,))
        out, _ = text(self.cap, "--streams")
        self.assertIn("1 gap, 100 bytes missing", out)
        follow = text(self.cap, "--follow", "0")[0]
        self.assertIn("[100 bytes missing from the capture]", follow)
        raw = self.follow(0, "client")
        self.assertEqual(raw, self.A[:300] + self.A[400:])  # the data that was seen, joined

    def test_lost_segment_found_later_by_a_retransmission(self):
        c = pb.Conversation()
        segs = [c.c2s(o, self.A[o:o + 100], flags=pb.ACK | pb.PSH) for o in range(0, 500, 100)]
        self.write(c.handshake() + [segs[0], segs[1], segs[3], segs[4], segs[2]] + c.close(500, 0))
        self.assertEqual(self.follow(0, "client"), self.A[:500])
        self.assertNotIn("gap", text(self.cap, "--streams")[0])

    def test_sequence_numbers_wrap_around(self):
        conv = pb.Conversation(isn_c=0xFFFFFF00 - 1, isn_s=0xFFFFFFF0)
        stream_cap(self, self.A, self.B, conv=conv, order=[2, 0, 1, 3, 4, 5, 6, 7, 8, 9] + list(range(10, 23)))
        self.check_data()

    def test_joining_mid_connection(self):
        c = pb.Conversation()
        frames = [c.c2s(5000, self.A[:400], flags=pb.ACK | pb.PSH), c.s2c(7000, self.B[:200], flags=pb.ACK | pb.PSH),
                  c.c2s(5400, self.A[400:], flags=pb.ACK | pb.PSH)]
        self.write(frames)
        self.assertEqual(self.follow(0, "client"), self.A)
        self.assertIn("midstream", text(self.cap, "--streams")[0])

    def test_reset_and_no_reply(self):
        c = pb.Conversation()
        self.write(c.handshake() + [c.c2s(0, b"hi", flags=pb.ACK | pb.PSH), c.s2c(0, flags=pb.RST | pb.ACK, ack_off=3)])
        self.assertIn("reset", text(self.cap, "--streams")[0])
        d = pb.Conversation(cport=41000)
        self.write([d.handshake()[0]])
        self.assertIn("no reply", text(self.cap, "--streams")[0])

    def test_data_after_the_syn_ack_and_roles(self):
        # the capture starts at the SYN-ACK: the sender is still the server
        c = pb.Conversation()
        h = c.handshake()
        self.write([h[1], h[2], c.c2s(0, b"hello", flags=pb.ACK | pb.PSH)])
        out = text(self.cap, "--streams")[0]
        row = [l for l in out.splitlines() if "40000" in l][0].split()
        self.assertEqual(row[1], "192.0.2.10.40000")  # the client is the side that did not send the SYN-ACK
        self.assertEqual(self.follow(0, "client"), b"hello")

    def test_ipv6_connection(self):
        conv = pb.Conversation(client=pb.ME6, server=pb.PEER6, v6=True)
        stream_cap(self, self.A, self.B, conv=conv, order=[1, 0] + list(range(2, 23)))
        self.check_data()
        self.assertIn("2001:db8::10.40000", text(self.cap, "--streams")[0])

    def test_two_connections_interleaved(self):
        c1, c2 = pb.Conversation(cport=40001), pb.Conversation(cport=40002, sport=8080)
        f1, f2 = case_frames(self.A, b"", conv=c1), case_frames(b"", self.B, conv=c2)
        merged = [x for pair in zip(f1, f2) for x in pair] + f1[len(f2):] + f2[len(f1):]
        self.write(merged)
        out = text(self.cap, "--streams")[0]
        self.assertIn("TCP connections (2)", out)
        self.assertEqual(run(self.cap, "--follow", "0", "--raw", "client").stdout, self.A)
        self.assertEqual(run(self.cap, "--follow", "1", "--raw", "server").stdout, self.B)

    def test_filter_limits_which_connections_are_tracked(self):
        c1, c2 = pb.Conversation(cport=40001), pb.Conversation(cport=40002, sport=8080)
        self.write(case_frames(self.A, b"", conv=c1) + case_frames(b"", self.B, conv=c2))
        out = text(self.cap, "--streams", "-f", "port 8080")[0]
        self.assertIn("TCP connections (1)", out)
        self.assertIn("8080", out)

    def test_a_port_pair_used_again_is_a_new_connection(self):
        first = case_frames(b"first connection", b"reply one", conv=pb.Conversation(isn_c=100, isn_s=200))
        second = case_frames(b"second one", b"reply two", conv=pb.Conversation(isn_c=5000, isn_s=6000))
        self.write(first + second)
        out = text(self.cap, "--streams")[0]
        self.assertIn("TCP connections (2)", out)
        self.assertEqual(run(self.cap, "--follow", "0", "--raw", "client").stdout, b"first connection")
        self.assertEqual(run(self.cap, "--follow", "1", "--raw", "server").stdout, b"reply two")

    def test_a_repeated_syn_is_not_a_new_connection(self):
        c = pb.Conversation()
        h = c.handshake()
        self.write([h[0], h[0], h[1], h[2], c.c2s(0, b"data", flags=pb.ACK | pb.PSH)])
        self.assertIn("TCP connections (1)", text(self.cap, "--streams")[0])

    def test_follow_text_shows_both_directions_in_order(self):
        c = pb.Conversation()
        frames = c.handshake() + [
            c.c2s(0, b"HELLO\r\n", flags=pb.ACK | pb.PSH), c.s2c(0, b"WORLD\r\n", flags=pb.ACK | pb.PSH, ack_off=8),
            c.c2s(7, b"BYE\r\n", flags=pb.ACK | pb.PSH, ack_off=8)]
        self.write(frames)
        out = text(self.cap, "--follow", "0")[0]
        self.assertLess(out.index("HELLO"), out.index("WORLD"))
        self.assertLess(out.index("WORLD"), out.index("BYE"))
        self.assertEqual(out.count("--- client -> server"), 2)
        self.assertEqual(out.count("--- server -> client"), 1)
        self.assertIn("(7 bytes)", out)

    def test_follow_text_escapes_binary(self):
        c = pb.Conversation()
        self.write(c.handshake() + [c.c2s(0, b"A\x00B\xffC\tD", flags=pb.ACK | pb.PSH)])
        out = text(self.cap, "--follow", "0")[0]
        self.assertIn("A\\x00B\\xffC\tD", out)

    def test_follow_hex(self):
        c = pb.Conversation()
        self.write(c.handshake() + [c.c2s(0, b"ABCDEFGHIJKLMNOPQR", flags=pb.ACK | pb.PSH)])
        out = text(self.cap, "--follow", "0", "-x")[0]
        self.assertIn("000000  41 42 43 44", out)
        self.assertIn("ABCDEFGHIJKLMNOP", out)
        self.assertIn("000010  51 52", out)

    def test_follow_missing_stream(self):
        stream_cap(self, b"x", b"y")
        _, err = text(self.cap, "--follow", "4", expect=1)
        self.assertIn("no stream 4", err)

    def test_raw_needs_follow(self):
        stream_cap(self, b"x", b"y")
        self.assertIn("--raw goes with --follow", text(self.cap, "--raw", "client", expect=2)[1])
        self.assertIn("cannot be combined", text(self.cap, "--follow", "0", "--raw", "client", "-s", expect=2)[1])

    def test_no_connections(self):
        self.write([pb.eth(pb.ipv4(pb.ME, pb.PEER, 17, pb.udp_datagram(pb.ME, pb.PEER, 1, 2, b"x")))])
        self.assertIn("no TCP connections", text(self.cap, "--streams")[0])
        self.assertIn("no HTTP found", text(self.cap, "--http")[0])

    def test_summary_and_streams_together(self):
        stream_cap(self, self.A, self.B)
        out = text(self.cap, "-s", "--streams")[0]
        self.assertIn("packets", out)
        self.assertIn("TCP connections (1)", out)

    def test_random_conversations_come_back_exactly(self):
        """Random data, random segment sizes, shuffled, with repeats: the bytes must match."""
        for seed in range(25):
            with self.subTest(seed=seed):
                rng = random.Random(seed)
                c2s = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 3000)))
                s2c = bytes(rng.randrange(256) for _ in range(rng.randrange(1, 3000)))
                conv = pb.Conversation(isn_c=rng.choice([1000, 0xFFFFF000, rng.randrange(1 << 32)]),
                                       isn_s=rng.choice([5000, 0xFFFFFF00, rng.randrange(1 << 32)]))
                segs = []
                for data, maker in ((c2s, conv.c2s), (s2c, conv.s2c)):
                    off = 0
                    while off < len(data):
                        n = rng.randrange(1, 600)
                        segs.append(maker(off, data[off:off + n], flags=pb.ACK | pb.PSH))
                        off += n
                    # repeat some segments, and resend a few as one bigger piece covering several
                    for _ in range(rng.randrange(0, 4)):
                        a = rng.randrange(0, len(data))
                        b = min(len(data), a + rng.randrange(1, 900))
                        segs.append(maker(a, data[a:b], flags=pb.ACK | pb.PSH))
                rng.shuffle(segs)
                # keep the handshake in front: joining mid-connection is a different case
                self.write(conv.handshake() + segs + conv.close(len(c2s), len(s2c)))
                self.assertEqual(run(self.cap, "--follow", "0", "--raw", "client").stdout, c2s)
                self.assertEqual(run(self.cap, "--follow", "0", "--raw", "server").stdout, s2c)


# ---- HTTP over streams ---------------------------------------------------------------------

def http_cap(case, client_data, server_data, seg=100):
    case.write(case_frames(client_data, server_data, seg=seg))


class TestHttp(Base):
    def http(self):
        return text(self.cap, "--http")[0]

    def test_request_and_response_split_across_segments(self):
        req = b"GET /a/long/path/that/spans/segments.html HTTP/1.1\r\nHost: example.test\r\nAccept: */*\r\n\r\n"
        body = b"<html>" + b"x" * 400 + b"</html>"
        resp = b"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\n\r\n" % len(body) + body
        http_cap(self, req, resp, seg=40)
        out = self.http()
        self.assertIn("GET /a/long/path/that/spans/segments.html", out)
        self.assertIn("[example.test]", out)
        self.assertIn("->  200 OK  text/html  %d bytes" % len(body), out)
        # the per-packet view cannot see the request line in a segment that starts mid-way
        self.assertNotIn("incomplete", out)

    def test_two_requests_on_one_connection(self):
        reqs = b"GET /one HTTP/1.1\r\nHost: h\r\n\r\nGET /two HTTP/1.1\r\nHost: h\r\n\r\n"
        resps = (b"HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\naaa" + b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        http_cap(self, reqs, resps)
        lines = [l for l in self.http().splitlines() if l.startswith("  stream")]
        self.assertEqual(len(lines), 2)
        self.assertIn("/one", lines[0])
        self.assertIn("200 OK", lines[0])
        self.assertIn("/two", lines[1])
        self.assertIn("404 Not Found", lines[1])

    def test_post_with_body(self):
        req = b"POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 11\r\n\r\nhello=world"
        resp = b"HTTP/1.1 201 Created\r\nContent-Length: 0\r\n\r\n"
        http_cap(self, req, resp)
        out = self.http()
        self.assertIn("POST /submit", out)
        self.assertIn("(sent 11 bytes)", out)
        self.assertIn("201 Created", out)

    def test_chunked_response(self):
        resp = (b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/plain\r\n\r\n"
                b"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")
        http_cap(self, b"GET /c HTTP/1.1\r\nHost: h\r\n\r\n", resp, seg=30)
        self.assertIn("text/plain  11 bytes", self.http())
        self.assertNotIn("incomplete", self.http())

    def test_head_response_has_no_body(self):
        reqs = b"HEAD /h HTTP/1.1\r\nHost: h\r\n\r\nGET /g HTTP/1.1\r\nHost: h\r\n\r\n"
        resps = b"HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n" + b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok"
        http_cap(self, reqs, resps)
        lines = [l for l in self.http().splitlines() if l.startswith("  stream")]
        self.assertIn("0 bytes", lines[0])
        self.assertIn("2 bytes", lines[1])

    def test_request_with_no_response(self):
        http_cap(self, b"GET /lonely HTTP/1.1\r\nHost: h\r\n\r\n", b"")
        self.assertIn("no response", self.http())

    def test_truncated_response_is_marked_incomplete(self):
        resp = b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n" + b"x" * 100
        http_cap(self, b"GET /big HTTP/1.1\r\nHost: h\r\n\r\n", resp)
        self.assertIn("100 bytes (incomplete)", self.http())

    def test_not_http_is_ignored(self):
        http_cap(self, b"\x16\x03\x01\x02\x00 not http at all", b"\x16\x03\x03 reply")
        self.assertIn("no HTTP found", self.http())

    def test_http_on_another_port(self):
        conv = pb.Conversation(sport=8080)
        self.write(case_frames(b"GET /p HTTP/1.1\r\nHost: h\r\n\r\n", b"HTTP/1.1 204 No Content\r\n\r\n", conv=conv))
        self.assertIn("204 No Content", self.http())

    def test_response_lost_in_the_capture_shows_what_arrived(self):
        resp = b"HTTP/1.1 200 OK\r\nContent-Length: 400\r\n\r\n" + b"y" * 400
        self.write(case_frames(b"GET /x HTTP/1.1\r\nHost: h\r\n\r\n", resp, seg=100, drop=(2,)))
        out = self.http()
        self.assertIn("GET /x", out)
        self.assertIn("200 OK", out)


class TestExamples(unittest.TestCase):
    CAP = os.path.join(os.path.dirname(HERE), "examples", "messy.pcap")

    def test_messy_example_matches_the_readme(self):
        out = text(self.CAP, "--streams")[0]
        self.assertIn("1 retransmitted, 1 out of order", out)
        self.assertIn("1 gap, 300 bytes missing", out)
        http = text(self.CAP, "--http")[0]
        self.assertIn("200 OK  text/html  1097 bytes", http)
        self.assertIn("797 bytes (incomplete)", http)
        self.assertIn("[reassembled from 3 fragments]", text(self.CAP, "-f", "udp")[0])
        page = run(self.CAP, "--follow", "0", "--raw", "server").stdout
        self.assertTrue(page.startswith(b"HTTP/1.1 200 OK") and page.endswith(b"</body></html>"))
        self.assertEqual(len(page), 1163)


class TestScale(Base):
    def test_many_conversations_stay_fast(self):
        import time
        frames = []
        for i in range(30000):
            src = f"10.{i >> 8 & 255}.{i & 255}.1"
            frames.append(pb.eth(pb.ipv4(src, pb.PEER, 6, pb.tcp_segment(src, pb.PEER, 1000 + i % 5000, 80, i, 0, pb.SYN))))
        self.write(frames)
        start = time.time()
        out = text(self.cap, "-s", "--streams")[0]
        self.assertLess(time.time() - start, 5.0)  # the summary once searched its flow list linearly, which made this quadratic
        self.assertIn("30000 in total", out)
        self.assertIn("TCP connections (30000)", out)


class TestRobustness(Base):
    def test_damaged_captures_never_crash(self):
        """Flip, drop and truncate bytes in a capture full of fragments and streams."""
        rng = random.Random(99)
        payload = pb.udp_datagram(pb.ME, pb.PEER, 5000, 6000, bytes(range(256)) * 8)
        frames = [pb.eth(f) for f in pb.fragment_v4(pb.ME, pb.PEER, 17, payload, 4, size=800)]
        pay6 = pb.udp_datagram(pb.ME6, pb.PEER6, 5000, 6000, bytes(range(256)) * 8, v6=True)
        frames += [pb.eth(f, 0x86DD) for f in pb.fragment_v6(pb.ME6, pb.PEER6, 17, pay6, 5, size=800)]
        req = b"GET /x HTTP/1.1\r\nHost: h\r\n\r\n"
        resp = b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n"
        frames += case_frames(req, resp, seg=20)
        for trial in range(250):
            damaged = []
            for f in frames:
                f = bytearray(f)
                for _ in range(rng.choice([0, 0, 0, 1, 3])):
                    if f:
                        f[rng.randrange(len(f))] = rng.randrange(256)
                if rng.random() < 0.1:
                    f = f[:rng.randrange(len(f) + 1)]
                if rng.random() < 0.05:
                    continue
                damaged.append(bytes(f))
            rng.shuffle(damaged) if trial % 3 == 0 else None
            self.write(damaged)
            r = subprocess.run([BIN, self.cap, "-s", "--streams", "--http", "--check"], capture_output=True, timeout=30)
            self.assertEqual(r.returncode, 0, (trial, r.stderr[-300:]))
            r = subprocess.run([BIN, self.cap, "--follow", "0"], capture_output=True, timeout=30)
            self.assertIn(r.returncode, (0, 1), (trial, r.stderr[-300:]))


if __name__ == "__main__":
    unittest.main()
