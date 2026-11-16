import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from capwriter import read_bytes, vectors, write_bytes, write_pcap, write_pcapng  # noqa: E402

BIN = os.path.join(ROOT, "pktscope")
V = vectors()
T0 = 1700000000.0  # 22:13:20 UTC


def run(*args, expect=0):
    r = subprocess.run([BIN, *args], capture_output=True, text=True, timeout=30)
    if expect is not None:
        assert r.returncode == expect, f"exit {r.returncode}, wanted {expect}\n{r.stdout}\n{r.stderr}"
    return r


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.cap = os.path.join(self.tmp.name, "t.pcap")

    def tearDown(self):
        self.tmp.cleanup()

    def conversation(self, names=("tcp_syn", "tcp_synack", "http_get", "http_resp", "dns_query", "dns_response",
                                  "arp_request", "icmp_echo")):
        write_pcap(self.cap, [(T0 + i * 0.25, V[n]) for i, n in enumerate(names)])


class TestOutput(Base):
    def test_one_line_per_packet(self):
        self.conversation()
        lines = run(self.cap).stdout.splitlines()
        self.assertEqual(len(lines), 8)
        self.assertEqual(lines[0], "22:13:20.000000 IP 10.0.0.5.51514 > 93.184.216.34.80: TCP [S] seq 1000 win 64240 len 0")
        self.assertEqual(lines[1], "22:13:20.250000 IP 93.184.216.34.80 > 10.0.0.5.51514: TCP [S.] seq 5000 ack 1001 win 65535 len 0")
        self.assertIn("| HTTP GET /index.html HTTP/1.1", lines[2])
        self.assertIn("| HTTP HTTP/1.1 200 OK", lines[3])
        self.assertIn("| DNS query id 4660 A www.example.com", lines[4])
        self.assertIn("=> CNAME web.example.net, A 192.0.2.7, AAAA 2001:db8::7", lines[5])
        self.assertTrue(lines[6].endswith("ARP who-has 10.0.0.1 tell 10.0.0.5"))
        self.assertIn("ICMP echo request id 119 seq 3", lines[7])

    def test_epoch_times(self):
        self.conversation(("tcp_syn",))
        self.assertTrue(run(self.cap, "--epoch").stdout.startswith("1700000000.000000 IP"))

    def test_count(self):
        self.conversation()
        self.assertEqual(len(run(self.cap, "-c", "3").stdout.splitlines()), 3)

    def test_hex_dump(self):
        self.conversation(("udp_plain",))
        out = run(self.cap, "-x").stdout
        self.assertIn("0000  68 65 6c 6c 6f 20 75 64  70", out)
        self.assertIn("hello udp", out)

    def test_bad_checksums_are_hidden_until_asked_for(self):
        self.conversation(("tcp_bad_checksum", "ip_bad_checksum", "tcp_syn"))
        self.assertNotIn("checksum", run(self.cap).stdout)
        lines = run(self.cap, "--check").stdout.splitlines()
        self.assertIn("[bad TCP checksum]", lines[0])
        self.assertIn("[bad IP checksum]", lines[1])
        self.assertNotIn("bad", lines[2])
        self.assertEqual(len(run(self.cap, "-f", "badcsum").stdout.splitlines()), 2)

    def test_truncated_packet_is_labelled(self):
        data = V["http_get"][:-20]
        write_pcap(self.cap, [(T0, data, len(V["http_get"]))])
        out = run(self.cap).stdout
        self.assertIn("[truncated]", out)


class TestFilter(Base):
    def setUp(self):
        super().setUp()
        self.conversation()

    def count(self, expr):
        return len(run(self.cap, "-f", expr).stdout.splitlines())

    def test_filters(self):
        self.assertEqual(self.count("tcp"), 4)
        self.assertEqual(self.count("udp"), 2)
        self.assertEqual(self.count("arp"), 1)
        self.assertEqual(self.count("port 80"), 4)
        self.assertEqual(self.count("dns"), 2)
        self.assertEqual(self.count("http"), 2)
        self.assertEqual(self.count("host 8.8.8.8"), 2)
        self.assertEqual(self.count("src host 8.8.8.8"), 1)
        self.assertEqual(self.count("net 10.0.0.0/8"), 8)      # every packet touches 10.0.0.5, ARP included
        self.assertEqual(self.count("net 93.184.0.0/16"), 4)
        self.assertEqual(self.count("tcp and not port 80"), 0)
        self.assertEqual(self.count("tcp and port 80 and not syn"), 3)
        self.assertEqual(self.count("(dns or arp) and not tcp"), 3)

    def test_filter_with_summary_and_json(self):
        out = run(self.cap, "-f", "dns", "--summary").stdout
        self.assertIn("2 packets", out)
        rows = [json.loads(l) for l in run(self.cap, "-f", "dns", "--json").stdout.splitlines()]
        self.assertEqual(len(rows), 2)

    def test_bad_filter(self):
        r = run(self.cap, "-f", "tcp and and", expect=2)
        self.assertIn("filter:", r.stderr)
        self.assertEqual(r.stdout, "")


class TestJson(Base):
    def test_every_line_is_valid_json_with_the_right_fields(self):
        self.conversation()
        rows = [json.loads(l) for l in run(self.cap, "--json").stdout.splitlines()]
        self.assertEqual(len(rows), 8)
        syn = rows[0]
        self.assertEqual((syn["src"], syn["dst"], syn["sport"], syn["dport"]), ("10.0.0.5", "93.184.216.34", 51514, 80))
        self.assertEqual((syn["flags"], syn["seq"], syn["win"], syn["ttl"], syn["proto"]), (2, 1000, 64240, 64, 6))
        self.assertEqual((syn["ip_csum"], syn["l4_csum"]), ("ok", "ok"))
        self.assertAlmostEqual(syn["ts"], T0)
        self.assertEqual(rows[2]["http"]["line"], "GET /index.html HTTP/1.1")
        self.assertEqual(rows[5]["dns"]["answers"][1], "A 192.0.2.7")
        self.assertEqual(rows[6]["arp"]["spa"], "10.0.0.5")
        self.assertEqual(rows[7]["icmp_type"], 8)

    def test_text_is_escaped(self):
        # an HTTP request line with a quote and a backslash in it
        base = bytearray(V["http_get"])
        base[54 + 4:54 + 8] = b'/a"\\'  # "GET /a"\..." keeps the packet length unchanged
        write_pcap(self.cap, [(T0, bytes(base))])
        row = json.loads(run(self.cap, "--json").stdout)
        self.assertIn('/a"\\', row["http"]["line"])


class TestSummary(Base):
    def test_summary(self):
        self.conversation()
        out = run(self.cap, "--summary").stdout
        self.assertIn("8 packets", out)
        self.assertIn("1.750 seconds", out)
        self.assertRegex(out, r"TCP\s+4")
        self.assertRegex(out, r"UDP\s+2")
        self.assertRegex(out, r"ICMP\s+1")
        self.assertIn("10.0.0.5.51514 <-> 93.184.216.34.80", out)
        self.assertIn("GET /index.html HTTP/1.1", out)
        self.assertIn("A www.example.com", out)

    def test_tcp_conversation_states(self):
        # SYN with no answer, a full open/close, and a reset
        self.conversation(("tcp_syn",))
        self.assertIn("no reply", run(self.cap, "--summary").stdout)
        self.conversation(("tcp_syn", "tcp_synack"))
        self.assertIn("open", run(self.cap, "--summary").stdout)

    def test_top_limits_rows(self):
        names = ["tcp_syn", "udp_plain", "icmp_echo", "dns_query", "ipv6_tcp"]
        self.conversation(names)
        out = run(self.cap, "--summary", "--top", "2").stdout
        rows = [l for l in out.splitlines() if "<->" in l]
        self.assertEqual(len(rows), 2)
        self.assertIn("(5 in total)", out)


class TestFormats(Base):
    def packets(self):
        return [(T0 + i, V[n]) for i, n in enumerate(("tcp_syn", "dns_query", "arp_request"))]

    def expected(self):
        return run(self.cap).stdout

    def test_every_file_flavour_reads_the_same(self):
        write_pcap(self.cap, self.packets())
        want = run(self.cap).stdout
        self.assertEqual(len(want.splitlines()), 3)
        variants = {
            "big-endian": lambda p: write_pcap(p, self.packets(), big_endian=True),
            "nanosecond": lambda p: write_pcap(p, self.packets(), nano=True),
            "big-endian nanosecond": lambda p: write_pcap(p, self.packets(), big_endian=True, nano=True),
            "pcapng": lambda p: write_pcapng(p, self.packets()),
            "pcapng big-endian": lambda p: write_pcapng(p, self.packets(), big_endian=True),
        }
        for name, make in variants.items():
            with self.subTest(name):
                path = os.path.join(self.tmp.name, "v.cap")
                make(path)
                self.assertEqual(run(path).stdout, want)

    def test_raw_ip_link_type(self):
        write_pcap(self.cap, [(T0, V["tcp_syn"][14:])], linktype=101)
        self.assertIn("IP 10.0.0.5.51514 > 93.184.216.34.80", run(self.cap).stdout)

    def test_unsupported_link_type_is_reported_not_fatal(self):
        write_pcap(self.cap, [(T0, b"\0" * 40), (T0 + 1, V["tcp_syn"])], linktype=105)
        out = run(self.cap).stdout.splitlines()
        self.assertEqual(len(out), 2)
        self.assertIn("link layer not decoded", out[0])


class TestWriteAndStdin(Base):
    def test_write_filtered_copy(self):
        self.conversation()
        out = os.path.join(self.tmp.name, "dns.pcap")
        r = run(self.cap, "-f", "dns", "-w", out, "-q")
        self.assertEqual(r.stdout, "")
        again = run(out).stdout.splitlines()
        self.assertEqual(again, run(self.cap, "-f", "dns").stdout.splitlines())
        self.assertEqual(len(again), 2)

    def test_write_without_quiet_also_prints(self):
        self.conversation()
        out = os.path.join(self.tmp.name, "all.pcap")
        r = run(self.cap, "-w", out)
        self.assertEqual(r.stdout, run(out).stdout)

    def test_convert_pcapng_to_pcap(self):
        names = ("tcp_syn", "http_get", "dns_query")
        ng = os.path.join(self.tmp.name, "t.pcapng")
        write_pcapng(ng, [(T0 + i, V[n]) for i, n in enumerate(names)])
        out = os.path.join(self.tmp.name, "c.pcap")
        run(ng, "-w", out, "-q")
        with open(out, "rb") as f:
            self.assertEqual(f.read(4), bytes.fromhex("d4c3b2a1"))  # classic pcap, little endian
        self.assertEqual(run(out).stdout, run(ng).stdout)

    def test_write_with_no_match_is_a_valid_empty_capture(self):
        self.conversation()
        out = os.path.join(self.tmp.name, "none.pcap")
        run(self.cap, "-f", "host 9.9.9.9", "-w", out, "-q")
        self.assertEqual(run(out).stdout, "")

    def test_write_to_unwritable_path(self):
        self.conversation()
        r = run(self.cap, "-w", "/nonexistent-dir/x.pcap", expect=1)
        self.assertIn("cannot write", r.stderr)

    def test_stdin_pcap_and_pcapng(self):
        self.conversation()
        want = run(self.cap).stdout
        with open(self.cap, "rb") as f:
            r = subprocess.run([BIN, "-"], stdin=f, capture_output=True, text=True, timeout=30)
        self.assertEqual((r.returncode, r.stdout), (0, want))
        ng = os.path.join(self.tmp.name, "t.pcapng")
        write_pcapng(ng, [(T0 + i, V[n]) for i, n in enumerate(("tcp_syn", "http_get"))])
        with open(ng, "rb") as f:
            r = subprocess.run([BIN, "-"], stdin=f, capture_output=True, text=True, timeout=30)
        self.assertEqual(r.stdout, run(ng).stdout)
        self.assertEqual(len(r.stdout.splitlines()), 2)

    def test_errors_name_the_file(self):
        junk = os.path.join(self.tmp.name, "junk")
        open(junk, "w").write("hello world, not a capture")
        self.assertIn(junk, run(junk, expect=1).stderr)
        empty = os.path.join(self.tmp.name, "empty")
        open(empty, "w").close()
        self.assertIn(empty, run(empty, expect=1).stderr)

    def test_number_out_of_range_message(self):
        self.conversation()
        self.assertIn("out of range", run(self.cap, "-f", "port 99999", expect=2).stderr)


class TestErrors(Base):
    def test_usage_errors(self):
        self.assertEqual(run(expect=2).returncode, 2)
        for args in (["--bogus", "x"], ["a.pcap", "b.pcap"], ["-c", "zero", "x"], ["-c", "0", "x"], ["-f"],
                     ["--live", "lo", "x"]):
            with self.subTest(args=args):
                r = run(*args, expect=2)
                self.assertTrue(r.stderr.startswith("pktscope:") or r.stderr.startswith("usage"), r.stderr)

    def test_help(self):
        r = run("--help")
        self.assertIn("usage: pktscope", r.stdout)
        self.assertIn("filter words", r.stdout)

    def test_missing_and_non_capture_files(self):
        r = run(os.path.join(self.tmp.name, "nope.pcap"), expect=1)
        self.assertIn("cannot open", r.stderr)
        write_bytes(self.cap, b"this is not a capture file at all")
        r = run(self.cap, expect=1)
        self.assertIn("not a capture file", r.stderr)
        write_bytes(self.cap, b"")
        self.assertIn("too short", run(self.cap, expect=1).stderr)

    def test_damaged_capture_keeps_the_packets_before_the_damage(self):
        write_pcap(self.cap, [(T0, V["tcp_syn"]), (T0 + 1, V["tcp_synack"]), (T0 + 2, V["http_get"])])
        data = read_bytes(self.cap)
        write_bytes(self.cap, data[:-15])
        r = run(self.cap, expect=1)
        self.assertEqual(len(r.stdout.splitlines()), 2)
        self.assertIn("middle of a packet", r.stderr)
        self.assertNotIn("Traceback", r.stderr)

    def test_random_damage_never_crashes_the_program(self):
        import random
        rnd = random.Random(4)
        write_pcap(self.cap, [(T0 + i, V[n]) for i, n in enumerate(V)])
        good = read_bytes(self.cap)
        for i in range(150):
            bad = bytearray(good)
            for _ in range(rnd.randint(1, 10)):
                bad[rnd.randrange(len(bad))] = rnd.randrange(256)
            if i % 5 == 0:
                bad = bad[:rnd.randrange(len(bad))]
            write_bytes(self.cap, bytes(bad))
            r = run(self.cap, "--json", expect=None)
            self.assertIn(r.returncode, (0, 1), r.stderr)  # a signal would be negative


def can_capture():
    try:
        s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(3))
        s.close()
        return True
    except (OSError, AttributeError):
        return False


@unittest.skipUnless(can_capture() or os.environ.get("REQUIRE_LIVE"), "live capture needs root on Linux")
class TestLive(Base):
    def test_capture_loopback_traffic(self):
        saved = os.path.join(self.tmp.name, "live.pcap")
        port = 0
        srv = socket.socket()
        self.addCleanup(srv.close)
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        port = srv.getsockname()[1]
        threading.Thread(target=lambda: (lambda c: (c.recv(100), c.sendall(b"pong"), c.close()))(srv.accept()[0]),
                         daemon=True).start()
        p = subprocess.Popen([BIN, "--live", "lo", "-w", saved, "-f", f"port {port}"], stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE, text=True)
        time.sleep(0.8)  # let it start listening
        c = socket.create_connection(("127.0.0.1", port))
        c.sendall(b"GET /live HTTP/1.1\r\n\r\n")
        c.recv(10)
        c.close()
        time.sleep(0.5)
        p.terminate()
        out, err = p.communicate(timeout=10)
        self.assertEqual(p.returncode, 0, err)
        self.assertIn("TCP [S]", out)
        self.assertIn("HTTP GET /live HTTP/1.1", out)
        # each packet appears once (not once per direction)
        self.assertEqual(sum("[S]" in l for l in out.splitlines()), 1)
        # and the saved file reads back the same
        again = run(saved).stdout
        self.assertEqual(again.splitlines(), out.splitlines())

    def test_bad_interface(self):
        r = run("--live", "no-such-if0", expect=1)
        self.assertIn("no such interface", r.stderr)


if __name__ == "__main__":
    unittest.main()
