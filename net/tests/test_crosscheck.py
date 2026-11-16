"""Compare pktscope with scapy on thousands of randomly built packets.

scapy builds each packet, then parses it back; pktscope's --json output for the same
bytes must agree on every field. Bad checksums are mixed in on purpose, and scapy's
own recomputation decides what the right verdict is.

Needs scapy (pip install scapy); skipped without it unless REQUIRE_REFERENCE=1.
"""

import json
import logging
import os
import random
import string
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

logging.getLogger("scapy.runtime").setLevel(logging.ERROR)
try:
    from scapy.all import (ARP, DNS, DNSQR, DNSRR, ICMP, IP, TCP, UDP, Dot1Q, Ether, ICMPv6EchoReply,
                           ICMPv6EchoRequest, IPv6, IPv6ExtHdrDestOpt, IPv6ExtHdrHopByHop, Raw, TCPOptions)
    HAVE = True
except ImportError:
    HAVE = False

if not HAVE and os.environ.get("REQUIRE_REFERENCE"):
    raise ImportError("REQUIRE_REFERENCE is set but scapy is not installed")

from capwriter import write_pcap  # noqa: E402

BIN = os.path.join(ROOT, "pktscope")
MAC = ["02:00:00:00:00:%02x" % i for i in range(1, 6)]


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def ip4(self):
        return ".".join(str(self.r.randint(1, 254)) for _ in range(4))

    def ip6(self):
        return ":".join("%x" % self.r.randint(0, 0xFFFF) for _ in range(8))

    def payload(self, hi=120):
        return Raw(bytes(self.r.randrange(256) for _ in range(self.r.randint(0, hi))))

    def name(self):
        labels = ["".join(self.r.choices(string.ascii_lowercase + string.digits, k=self.r.randint(1, 12)))
                  for _ in range(self.r.randint(1, 4))]
        return ".".join(labels + [self.r.choice(["com", "net", "org", "io"])])

    def corrupt(self, layer):
        """Sometimes overwrite the checksum with a wrong value."""
        if self.r.random() < 0.2:
            layer.chksum = self.r.randrange(1, 65536)
        return layer

    def ip4_layer(self, proto=None):
        kw = dict(src=self.ip4(), dst=self.ip4(), ttl=self.r.randint(1, 255), id=self.r.randrange(65536))
        if proto:
            kw["proto"] = proto
        if self.r.random() < 0.25:
            kw["options"] = [__import__("scapy.all", fromlist=["IPOption_NOP"]).IPOption_NOP() for _ in range(self.r.choice([4, 8]))]
        return IP(**kw)

    def tcp(self):
        opts = []
        if self.r.random() < 0.5:
            opts = [("MSS", self.r.randint(500, 1460)), ("NOP", None), ("WScale", self.r.randint(0, 10)),
                    ("SAckOK", b""), ("Timestamp", (self.r.randrange(2**32), self.r.randrange(2**32)))]
        return TCP(sport=self.r.randint(1, 65535), dport=self.r.choice([80, 443, 22, 8080, self.r.randint(1, 65535)]),
                   seq=self.r.randrange(2**32), ack=self.r.randrange(2**32), flags=self.r.randrange(256),
                   window=self.r.randrange(65536), options=opts)

    def dns(self):
        q = DNSQR(qname=self.name(), qtype=self.r.choice(["A", "AAAA", "MX", "TXT", "NS", "CNAME", "PTR"]))
        if self.r.random() < 0.5:
            return DNS(id=self.r.randrange(65536), rd=1, qd=q)
        answers = []
        for _ in range(self.r.randint(0, 3)):
            kind = self.r.choice(["A", "AAAA", "CNAME"])
            data = {"A": self.ip4, "AAAA": self.ip6, "CNAME": self.name}[kind]()
            answers.append(DNSRR(rrname=q.qname, type=kind, ttl=self.r.randrange(100000), rdata=data))
        d = DNS(id=self.r.randrange(65536), qr=1, rd=1, ra=1, rcode=self.r.choice([0, 0, 0, 3, 2]), qd=q, ancount=len(answers))
        if answers:
            chain = answers[0]
            for a in answers[1:]:
                chain = chain / a
            d.an = chain
        return d

    def l3(self):
        k = self.r.choice(["tcp4", "tcp4", "udp4", "dns4", "dns4", "icmp4", "tcp6", "udp6", "icmp6", "ext6", "arp", "frag4"])
        if k == "tcp4":
            return self.ip4_layer() / self.corrupt(self.tcp()) / self.payload()
        if k == "udp4":
            u = UDP(sport=self.r.randint(1, 65535), dport=self.r.randint(1, 65535))
            if self.r.random() < 0.15:
                u.chksum = 0
            return self.ip4_layer() / self.corrupt(u) / self.payload()
        if k == "dns4":
            ports = (self.r.randint(1024, 65535), 53)
            if self.r.random() < 0.5:
                ports = ports[::-1]
            return self.ip4_layer() / self.corrupt(UDP(sport=ports[0], dport=ports[1])) / self.dns()
        if k == "icmp4":
            t = self.r.choice([8, 0, 3, 11])
            return self.ip4_layer() / self.corrupt(ICMP(type=t, code=self.r.randint(0, 3), id=self.r.randrange(65536),
                                                         seq=self.r.randrange(65536))) / self.payload(60)
        if k == "tcp6":
            return IPv6(src=self.ip6(), dst=self.ip6(), hlim=self.r.randint(1, 255)) / self.corrupt(self.tcp()) / self.payload()
        if k == "udp6":
            return IPv6(src=self.ip6(), dst=self.ip6(), hlim=self.r.randint(1, 255)) / self.corrupt(
                UDP(sport=self.r.randint(1, 65535), dport=self.r.randint(1, 65535))) / self.payload()
        if k == "icmp6":
            cls = self.r.choice([ICMPv6EchoRequest, ICMPv6EchoReply])
            return IPv6(src=self.ip6(), dst=self.ip6(), hlim=self.r.randint(1, 255)) / cls(
                id=self.r.randrange(65536), seq=self.r.randrange(65536), data=bytes(self.r.randrange(256) for _ in range(self.r.randint(0, 50))))
        if k == "ext6":
            ext = self.r.choice([IPv6ExtHdrHopByHop(), IPv6ExtHdrDestOpt(), IPv6ExtHdrHopByHop() / IPv6ExtHdrDestOpt()])
            return IPv6(src=self.ip6(), dst=self.ip6()) / ext / UDP(sport=self.r.randint(1, 65535), dport=self.r.randint(1, 65535)) / self.payload()
        if k == "arp":
            return ARP(op=self.r.choice([1, 2]), hwsrc=self.r.choice(MAC), psrc=self.ip4(), hwdst=self.r.choice(MAC), pdst=self.ip4())
        # a later fragment of a fragmented datagram
        return IP(src=self.ip4(), dst=self.ip4(), id=self.r.randrange(65536), frag=self.r.randint(1, 100), proto=17,
                  flags=self.r.choice([0, "MF"])) / self.payload(80)

    def frame(self):
        l3 = self.l3()
        e = Ether(src=self.r.choice(MAC), dst=self.r.choice(MAC))
        r = self.r.random()
        if r < 0.15:
            e = e / Dot1Q(vlan=self.r.randint(1, 4094))
        elif r < 0.2:
            e = e / Dot1Q(vlan=self.r.randint(1, 4094)) / Dot1Q(vlan=self.r.randint(1, 4094))
        raw = bytes(e / l3)
        if len(raw) < 60 and self.r.random() < 0.7:
            raw += b"\0" * (60 - len(raw))  # what a real network card does to short frames
        return raw


def recomputed_ok(raw, layer_cls):
    """Is the checksum in `layer_cls` right? Delete it, let scapy rebuild, and compare."""
    p = Ether(raw)
    if layer_cls not in p:
        return None
    original = p[layer_cls].chksum if hasattr(p[layer_cls], "chksum") else p[layer_cls].cksum
    name = "chksum" if hasattr(p[layer_cls], "chksum") else "cksum"
    q = Ether(raw)
    delattr(q[layer_cls], name)
    # re-serialise only up to the end of the datagram so Ethernet padding doesn't confuse the rebuild
    rebuilt = Ether(bytes(q))
    return getattr(rebuilt[layer_cls], name) == original


def expected(raw):
    """What scapy says is in this frame, in the shape of pktscope's JSON."""
    p = Ether(raw)
    out = {"eth_src": p.src, "eth_dst": p.dst}
    layer = p.payload
    vlan = None
    while isinstance(layer, Dot1Q):
        vlan = layer.vlan if vlan is None else vlan
        layer = layer.payload
    if vlan is not None:
        out["vlan"] = vlan
    if isinstance(layer, ARP):
        out["arp"] = {"op": layer.op, "sha": layer.hwsrc, "spa": layer.psrc, "tha": layer.hwdst, "tpa": layer.pdst}
        return out
    if isinstance(layer, IP):
        ip = layer
        out.update(ip=4, src=ip.src, dst=ip.dst, ttl=ip.ttl, proto=ip.proto)
        out["ip_csum"] = "ok" if recomputed_ok(raw, IP) else "bad"
        body = ip.len - ip.ihl * 4
        if ip.frag > 0:
            out["frag_offset"] = ip.frag * 8
            return out
        t = ip.payload
        if int(ip.flags) & 1:
            out["frag_offset"] = 0
    elif isinstance(layer, IPv6):
        ip6 = layer
        out.update(ip=6, src=ip6.src, dst=ip6.dst, ttl=ip6.hlim)
        t = ip6.payload
        ext = 0
        while isinstance(t, (IPv6ExtHdrHopByHop, IPv6ExtHdrDestOpt)):
            ext += len(bytes(t)) - len(bytes(t.payload))
            t = t.payload
        body = ip6.plen - ext
        out["ip_csum"] = "na"
    else:
        return out
    if isinstance(t, TCP):
        out.update(proto=6, sport=t.sport, dport=t.dport, seq=t.seq, ack=t.ack, flags=int(t.flags), win=t.window,
                   payload=body - t.dataofs * 4, l4_csum="ok" if recomputed_ok(raw, TCP) else "bad")
    elif isinstance(t, UDP):
        out.update(proto=17, sport=t.sport, dport=t.dport, payload=t.len - 8)
        if out["ip"] == 4 and t.chksum == 0:
            out["l4_csum"] = "na"
        else:
            out["l4_csum"] = "ok" if recomputed_ok(raw, UDP) else "bad"
        d = t.getlayer(DNS)
        if d is not None and 53 in (t.sport, t.dport):
            dns = {"id": d.id, "qr": d.qr, "rcode": d.rcode, "qd": d.qdcount, "an": d.ancount}
            if d.qdcount:
                dns["qname"] = d.qd.qname.decode().rstrip(".")
                dns["qtype"] = d.qd.qtype
            answers = []
            for rr in list(d.an or [])[:4]:  # pktscope decodes at most four answers
                data = rr.rdata.decode().rstrip(".") if isinstance(rr.rdata, bytes) else rr.rdata
                answers.append({1: "A", 28: "AAAA", 5: "CNAME"}[rr.type] + " " + str(data))
            dns["answers"] = answers
            out["dns"] = dns
    elif isinstance(t, ICMP):
        out.update(proto=1, icmp_type=t.type, icmp_code=t.code, l4_csum="ok" if recomputed_ok(raw, ICMP) else "bad")
        if t.type in (0, 8):
            out.update(icmp_id=t.id, icmp_seq=t.seq)
    elif isinstance(t, (ICMPv6EchoRequest, ICMPv6EchoReply)):
        out.update(proto=58, icmp_type=t.type, icmp_code=t.code, icmp_id=t.id, icmp_seq=t.seq)
        out["l4_csum"] = "ok" if recomputed_ok(raw, type(t)) else "bad"
    return out


def compare(want, got):
    """Every key scapy reported must match; returns a list of differences."""
    diffs = []
    for k, v in want.items():
        if k in ("arp",):
            for kk, vv in v.items():
                if got.get("arp", {}).get(kk) != vv:
                    diffs.append(f"arp.{kk}: scapy {vv!r} / pktscope {got.get('arp', {}).get(kk)!r}")
        elif k == "dns":
            for kk, vv in v.items():
                if got.get("dns", {}).get(kk) != vv:
                    diffs.append(f"dns.{kk}: scapy {vv!r} / pktscope {got.get('dns', {}).get(kk)!r}")
        elif got.get(k) != v:
            diffs.append(f"{k}: scapy {v!r} / pktscope {got.get(k)!r}")
    return diffs


@unittest.skipUnless(HAVE, "scapy not installed")
class TestAgainstScapy(unittest.TestCase):
    def run_seed(self, seed, count):
        gen = Gen(seed)
        frames = [gen.frame() for _ in range(count)]
        with tempfile.TemporaryDirectory() as d:
            cap = os.path.join(d, "x.pcap")
            write_pcap(cap, [(1700000000 + i * 0.001, f) for i, f in enumerate(frames)])
            r = subprocess.run([BIN, "--json", cap], capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr)
        rows = [json.loads(line) for line in r.stdout.splitlines()]
        self.assertEqual(len(rows), count)
        bad = []
        for i, (frame, row) in enumerate(zip(frames, rows)):
            want = expected(frame)
            want["len"] = want["caplen"] = len(frame)
            diffs = compare(want, row)
            if abs(row["ts"] - (1700000000 + i * 0.001)) > 1e-6:
                diffs.append(f"ts {row['ts']}")
            if diffs:
                bad.append(f"packet {i} ({Ether(frame).summary()}):\n    " + "\n    ".join(diffs))
        self.assertEqual(bad[:5], [], f"{len(bad)} of {count} packets disagree")

    def test_random_packets_seed_1(self):
        self.run_seed(1, 1500)

    def test_random_packets_seed_2(self):
        self.run_seed(2, 1500)

    def test_random_packets_seed_3(self):
        self.run_seed(3, 1500)


if __name__ == "__main__":
    unittest.main()
