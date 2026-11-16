"""Minimal capture-file writers for tests (no third-party libraries)."""

import re
import struct
import os

HERE = os.path.dirname(os.path.abspath(__file__))


def vectors():
    """The scapy-built packets from vectors.h, as {name: bytes}."""
    with open(os.path.join(HERE, "vectors.h")) as f:
        text = f.read()
    return {n: bytes.fromhex(h) for n, h in re.findall(r'\{"(\w+)", "([0-9a-f]+)"\}', text)}


def write_pcap(path, packets, linktype=1, big_endian=False, nano=False, snaplen=65535):
    """packets: list of (timestamp_seconds, bytes[, original_length])."""
    e = ">" if big_endian else "<"
    magic = 0xA1B23C4D if nano else 0xA1B2C3D4
    out = bytearray(struct.pack(e + "IHHiIII", magic, 2, 4, 0, 0, snaplen, linktype))
    for item in packets:
        ts, data = item[0], item[1]
        orig = item[2] if len(item) > 2 else len(data)
        sec = int(ts)
        frac = round((ts - sec) * (1e9 if nano else 1e6))
        out += struct.pack(e + "IIII", sec, frac, len(data), orig) + data
    with open(path, "wb") as f:
        f.write(out)


def write_pcapng(path, packets, linktype=1, big_endian=False):
    """A pcapng file with one Ethernet interface at microsecond resolution."""
    e = ">" if big_endian else "<"

    def block(kind, body):
        body += b"\0" * (-len(body) % 4)
        total = 12 + len(body)
        return struct.pack(e + "II", kind, total) + body + struct.pack(e + "I", total)

    out = block(0x0A0D0D0A, struct.pack(e + "IHHq", 0x1A2B3C4D, 1, 0, -1))
    out += block(1, struct.pack(e + "HHI", linktype, 0, 65535))
    for ts, data in packets:
        ticks = round(ts * 1e6)
        out += block(6, struct.pack(e + "IIIII", 0, ticks >> 32, ticks & 0xFFFFFFFF, len(data), len(data)) + data)
    with open(path, "wb") as f:
        f.write(out)


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()
