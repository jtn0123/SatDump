#!/usr/bin/env python3
"""Make the small synthetic recordings for the golden tests. Nothing here is received data.

  make_recordings.py <output folder>

  metop.cadu  MetOp-B AHRPT transfer frames (CADUs, after error correction): AVHRR/3 lines
              in CCSDS packets, cut across frame boundaries the way the satellite does it,
              mixed with fill frames and one admin message carrying a TLE
  noaa.raw16  NOAA-19 HRPT minor frames (16-bit words): AVHRR/3 lines, time codes and
              calibration views

The picture is a fixed pattern made with integer arithmetic from fixed seeds, so both files
are byte-for-byte the same on every machine and Python 3 version. Only the standard
library is used.
"""

import bz2
import os
import random
import struct
import sys

LINES = 96
WIDTH = 2048
START_DAY = 9445  # days after 2000-01-01: 2025-11-10
START_MS = 12 * 3600 * 1000  # 12:00:00 UTC


def picture(seed, lines=LINES, width=WIDTH):
    """A smooth cloud-like field, 10-bit counts 60..960, one row per line."""
    rng = random.Random(seed)
    rows = [[0] * width for _ in range(lines)]
    # Value noise at three scales, bilinear between grid points, all integer
    for cell, weight in ((256, 5), (64, 3), (16, 2)):
        gw, gh = width // cell + 2, lines // cell + 2
        grid = [[rng.randrange(1000) for _ in range(gw)] for _ in range(gh)]
        for y in range(lines):
            gy, fy = divmod(y, cell)
            top, bot = grid[gy], grid[gy + 1]
            row = rows[y]
            for x in range(width):
                gx, fx = divmod(x, cell)
                a = top[gx] * (cell - fx) + top[gx + 1] * fx
                b = bot[gx] * (cell - fx) + bot[gx + 1] * fx
                row[x] += weight * (a * (cell - fy) + b * fy) // (cell * cell)
    lo = min(min(r) for r in rows)
    hi = max(max(r) for r in rows)
    return [[60 + (v - lo) * 900 // (hi - lo) for v in r] for r in rows]


def channels():
    """Five AVHRR channels: two visible, then three thermal (clouds are cold, so darker)."""
    out = []
    for ch in range(5):
        p = picture(100 + ch)
        if ch >= 2:
            p = [[1020 - v * 4 // 5 for v in r] for r in p]
        out.append(p)
    return out


# ------------------------------------------------------------------------------- MetOp


def pack10(words):
    """10-bit words, most significant bit first, padded with zero bits to whole bytes."""
    acc = nbits = 0
    out = bytearray()
    for w in words:
        acc = (acc << 10) | (w & 0x3FF)
        nbits += 10
        while nbits >= 8:
            nbits -= 8
            out.append((acc >> nbits) & 0xFF)
        acc &= (1 << nbits) - 1
    if nbits:
        out.append((acc << (8 - nbits)) & 0xFF)
    return bytes(out)


def ccsds_packet(apid, seq, payload, secondary=True):
    hdr = struct.pack(">HHH", (int(secondary) << 11) | apid, (3 << 14) | (seq & 0x3FFF), len(payload) - 1)
    return hdr + payload


def with_vertical_parity(apid, seq, body):
    """A packet whose last 16-bit word is the XOR of all the words before it."""
    pkt = bytearray(ccsds_packet(apid, seq, body + b"\0\0"))
    x = 0
    for i in range(0, len(pkt) - 2, 2):
        x ^= pkt[i] << 8 | pkt[i + 1]
    pkt[-2:] = struct.pack(">H", x)
    return bytes(pkt)


def avhrr_packets(ch):
    pkts = []
    for line in range(LINES):
        words = [0] * 10355
        for i in range(10):  # space view (above 880 for the three thermal channels)
            words[5 * i : 5 * i + 5] = (40, 40, 990, 990, 990)
        for c in range(5):
            words[55 + c : 55 + 5 * WIDTH : 5] = ch[c][line]
        words[10295 : 10300] = (0, 0, 300, 301, 302)  # platinum thermometers
        for i in range(10):  # blackbody view
            words[10305 + 5 * i : 10310 + 5 * i] = (0, 0, 420, 430, 440)
        ms = START_MS + line * 1000 // 6  # six lines a second
        body = struct.pack(">HIH", START_DAY, ms, 0) + bytes(6) + pack10(words)
        assert len(body) == 12958
        pkts.append(with_vertical_parity(103, line, body))
    return pkts


def checksum(line):
    return str(sum(int(c) if c.isdigit() else (c == "-") for c in line[:68]) % 10)


def admin_packet():
    l1 = "1 38771U 12049A   25314.50000000  .00000000  00000-0  00000-0 0  999"
    l2 = "2 38771  98.7000 300.0000 0001000  90.0000 270.0000 14.21000000 0000"
    assert len(l1) == len(l2) == 68
    l1 += checksum(l1)
    l2 += checksum(l2)
    xml = ('<multi-mission-administrative-message><message satellite="M01" satellite-number="38771">'
           f"<navigation><two-line-elements><line-1>{l1}</line-1><line-2>{l2}</line-2>"
           "</two-line-elements></navigation></message></multi-mission-administrative-message>")
    return ccsds_packet(6, 1, bytes(41) + bz2.compress(xml.encode(), 9))


SCID = 11  # MetOp-B
DATA = 882  # M_PDU packet zone of a MetOp CADU


class VirtualChannel:
    """Cuts a packet stream into M_PDUs: packets run on across frames, and the first header
    pointer says where the first packet starting in a frame begins (2047: none starts)."""

    def __init__(self, vcid):
        self.vcid, self.count, self.buf, self.starts = vcid, 0, bytearray(), []

    def add(self, pkt):
        self.starts.append(len(self.buf))
        self.buf += pkt

    def frames(self):
        # Fill the last frame with an idle packet (APID 2047), as the satellite does
        pad = -len(self.buf) % DATA
        if pad:
            self.add(ccsds_packet(2047, 0, bytes(pad - 6 if pad >= 7 else pad + DATA - 6), secondary=False))
        out, pos = [], 0
        while pos < len(self.buf):
            zone = self.buf[pos : pos + DATA]
            fhp = next((s - pos for s in self.starts if pos <= s < pos + DATA), 2047)
            out.append(self.cadu(fhp, zone))
            pos += DATA
        return out

    def cadu(self, fhp, zone):
        c = self.count
        self.count += 1
        vcdu = struct.pack(">HBHB", (1 << 14) | (SCID << 6) | self.vcid, c >> 16, c & 0xFFFF, 0)
        # Sync marker, VCDU header, 2-byte insert zone, M_PDU header, packets, RS check bytes
        return b"\x1a\xcf\xfc\x1d" + vcdu + b"\0\0" + struct.pack(">H", fhp) + zone + bytes(128)


def metop_cadus(ch):
    avhrr = VirtualChannel(9)
    for p in avhrr_packets(ch):
        avhrr.add(p)
    admin = VirtualChannel(34)
    admin.add(admin_packet())
    fill = VirtualChannel(63)
    frames = avhrr.frames()
    out = []
    for i, f in enumerate(frames):
        out.append(f)
        if i % 17 == 16:  # some fill frames between the data
            out.append(fill.cadu(2047, bytes(DATA)))
        if i == len(frames) // 2:
            out += admin.frames()
    assert all(len(c) == 1024 for c in out)
    return b"".join(out)


# -------------------------------------------------------------------------------- NOAA


def noaa_frames(ch):
    out = bytearray()
    for line in range(LINES):
        f = [0] * 11090
        f[0:6] = (0x284, 0x16F, 0x35C, 0x19D, 0x20F, 0x095)  # frame sync
        f[6] = (15 << 3) | ((line % 3 + 1) << 7)  # spacecraft 15 (NOAA-19), minor frame 1 to 3
        ms = START_MS + line * 1000 // 6
        f[8] = (314 << 1) & 0x3FF  # day 314 of the year
        f[9], f[10], f[11] = (ms >> 20) & 0x7F, (ms >> 10) & 0x3FF, ms & 0x3FF
        f[12:17] = [520] * 5  # ramp calibration
        f[17:20] = (0, 0, 0) if line % 5 == 0 else (300, 301, 302)  # thermometer reference mark
        f[22:52] = (420, 430, 440) * 10  # blackbody view
        f[52:102] = (40, 40, 990, 990, 990) * 10  # space view
        for c in range(5):
            f[750 + c : 750 + 5 * WIDTH : 5] = ch[c][line]
        f[10990:] = [0x155] * 100
        out += struct.pack("<11090H", *f)
    return bytes(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    ch = channels()
    for name, data in (("metop.cadu", metop_cadus(ch)), ("noaa.raw16", noaa_frames(ch))):
        with open(os.path.join(sys.argv[1], name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
