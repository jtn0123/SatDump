#!/usr/bin/env python3
"""Make the small synthetic recordings for the golden tests. Nothing here is received data.

  make_recordings.py <output folder>

  metop.cadu  MetOp-B AHRPT transfer frames (CADUs, after error correction): AVHRR/3 lines
              in CCSDS packets, cut across frame boundaries the way the satellite does it,
              mixed with fill frames and one admin message carrying a TLE
  noaa.raw16  NOAA-19 HRPT minor frames (16-bit words): AVHRR/3 lines, time codes and
              calibration views
  meteor.cadu Meteor-M N2-3 LRPT transfer frames (CADUs, after error correction): three MSU-MR
              channels compressed the JPEG way, with telemetry, a lost packet, a damaged
              packet and two short telemetry packets, as happens in a weak pass

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

    def __init__(self, vcid, scid=SCID):
        self.vcid, self.scid, self.count, self.buf, self.starts = vcid, scid, 0, bytearray(), []

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
        vcdu = struct.pack(">HBHB", (1 << 14) | (self.scid << 6) | self.vcid, c >> 16, c & 0xFFFF, 0)
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


# ------------------------------------------------------------------------------ Meteor

# MSU-MR pictures are sent as JPEG-style segments: every 8-line strip of a channel is 14
# packets, each with 14 blocks of 8 x 8 pixels. These are the standard JPEG luminance tables
# (ITU-T T.81 annex K), written out here independently of the decoder's own copy.
Q_LUMA = (16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
          14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
          18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
          49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99)
DC_BITS = (0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0)  # codes per length, 1 to 16 bits
AC_BITS = (0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125)
AC_VALS = bytes.fromhex("""
    01 02 03 00 04 11 05 12 21 31 41 06 13 51 61 07 22 71 14 32 81 91 a1 08 23 42 b1 c1 15 52 d1 f0
    24 33 62 72 82 09 0a 16 17 18 19 1a 25 26 27 28 29 2a 34 35 36 37 38 39 3a 43 44 45 46 47 48 49
    4a 53 54 55 56 57 58 59 5a 63 64 65 66 67 68 69 6a 73 74 75 76 77 78 79 7a 83 84 85 86 87 88 89
    8a 92 93 94 95 96 97 98 99 9a a2 a3 a4 a5 a6 a7 a8 a9 aa b2 b3 b4 b5 b6 b7 b8 b9 ba c2 c3 c4 c5
    c6 c7 c8 c9 ca d2 d3 d4 d5 d6 d7 d8 d9 da e1 e2 e3 e4 e5 e6 e7 e8 e9 ea f1 f2 f3 f4 f5 f6 f7 f8
    f9 fa""")  # (zero run << 4) | size of each AC code, in code order
assert sorted(AC_VALS) == sorted([0, 0xF0] + [r << 4 | z for r in range(16) for z in range(1, 11)])
QF = 80  # quality factor: the table is Q_LUMA scaled by (200 - 2 * QF) percent, rounded
QTAB = [max(1, (4 * q + 5) // 10) for q in Q_LUMA]
ZIGZAG = sorted(range(64), key=lambda i: (i // 8 + i % 8, i // 8 if (i // 8 + i % 8) % 2 else i % 8))
COS16 = (4096, 4017, 3784, 3406, 2896, 2276, 1567, 799)  # 4096 * cos(k * pi / 16), rounded
METEOR_DAY = 9080  # Meteor counts days from 2000-12-31: 2025-11-10
STRIPS = 8  # 64 lines


def huffman_codes(bits, vals):
    codes, code, k = {}, 0, 0
    for length, count in enumerate(bits, 1):
        for _ in range(count):
            codes[vals[k]] = (code, length)
            code, k = code + 1, k + 1
        code <<= 1
    return codes


DC_CODES = huffman_codes(DC_BITS, range(12))
AC_CODES = huffman_codes(AC_BITS, AC_VALS)


def cos16(m):
    m %= 32
    m = 32 - m if m > 16 else m
    return COS16[m] if m < 8 else -COS16[16 - m] if m > 8 else 0


# The DCT basis, 4096 times too big; the first row also carries the 1 / sqrt(2) of the DCT
BASIS = [[cos16((2 * x + 1) * u) if u else 2896 for x in range(8)] for u in range(8)]


def rdiv(a, b):
    """a / b rounded to the nearest whole number, halves away from zero (b > 0)."""
    return (2 * a + b) // (2 * b) if a >= 0 else -((b - 2 * a) // (2 * b))


def quantized_dct(px):
    """8 x 8 pixels (rows, 0 to 255) to quantized DCT coefficients in zig-zag order."""
    rows = [[sum(b * (p - 128) for b, p in zip(BASIS[u], px[8 * y : 8 * y + 8])) for u in range(8)] for y in range(8)]
    out = [0] * 64
    for v in range(8):
        for u in range(8):
            out[8 * v + u] = rdiv(sum(BASIS[v][y] * rows[y][u] for y in range(8)), 4 * 4096 * 4096 * QTAB[8 * v + u])
    zz = [out[i] for i in ZIGZAG]
    zz[63] = 0  # the decoder never stores the last coefficient, so it is never sent
    return zz


class BitWriter:
    def __init__(self):
        self.acc = self.n = 0

    def put(self, code, n):
        self.acc, self.n = (self.acc << n) | (code & ((1 << n) - 1)), self.n + n

    def value(self, v):
        """A JPEG magnitude: its size in bits, then the bits (negative values one less)."""
        s = abs(v).bit_length()
        self.put(v if v >= 0 else v + (1 << s) - 1, s)
        return s

    def data(self):
        pad = -self.n % 8  # padded with one bits, as in JPEG
        return ((self.acc << pad) | ((1 << pad) - 1)).to_bytes((self.n + pad) // 8, "big")


def encode_blocks(blocks):
    """Huffman-code blocks of zig-zag coefficients; the DC value is sent as a difference."""
    w, last = BitWriter(), 0
    for zz in blocks:
        diff, last = zz[0] - last, zz[0]
        w.put(*DC_CODES[abs(diff).bit_length()])
        w.value(diff)
        nonzero = [k for k in range(1, 64) if zz[k]]
        run = 0
        for k in range(1, nonzero[-1] + 1 if nonzero else 1):
            if zz[k] == 0:
                run += 1
                continue
            while run > 15:
                w.put(*AC_CODES[0xF0])  # sixteen zeros
                run -= 16
            size = abs(zz[k]).bit_length()
            assert size <= 10
            w.put(*AC_CODES[run << 4 | size])
            w.value(zz[k])
            run = 0
        w.put(*AC_CODES[0x00])  # end of block
    return w.data()


def meteor_time(strip):
    return struct.pack(">HIH", METEOR_DAY, START_MS + strip * 1232, 0)  # about 1.23 s a strip


def meteor_channels():
    """Three 8-bit channels with thin bright bands, whose sharp edges need many coefficients."""
    out = []
    for c in range(3):
        p = picture(200 + c, lines=8 * STRIPS, width=1568)
        out.append([[min(255, (v - 60) * 220 // 900 + 16 + 120 * ((x + 3 * y + 50 * c) % 397 < 24))
                     for x, v in enumerate(r)] for y, r in enumerate(p)])
    return out


def meteor_cadus():
    ch = meteor_channels()
    vc = VirtualChannel(5, scid=0)  # the decoder uses virtual channel 5 and ignores the SCID
    seq = 16384 - 3 * 43  # the 14-bit packet counter wraps in the fourth strip
    for strip in range(STRIPS):
        # A strip is 43 packets: 14 for each channel, then one telemetry packet
        for c in range(3):
            for m in range(14):
                blocks = [quantized_dct([ch[c][8 * strip + y][112 * m + 8 * b + x] for y in range(8) for x in range(8)])
                          for b in range(14)]
                if (c, strip, m) == (0, 1, 3):  # one block with long zero runs and the last coefficient sent
                    blocks[0] = [blocks[0][0]] + [0] * 24 + [3] + [0] * 36 + [-2, 0]
                head = meteor_time(strip) + bytes((14 * m, 0, 0)) + struct.pack(">HB", 0xFFF0, QF)
                if (c, strip, m) == (2, 2, 9):  # damaged after nine blocks: the rest stays black
                    body = encode_blocks(blocks[:9]) + b"\xff\xff"
                else:
                    body = encode_blocks(blocks)
                if (c, strip, m) != (1, 5, 6):  # this packet is lost on the way
                    vc.add(ccsds_packet(64 + c, seq, head + body))
                seq += 1
        # Telemetry: MSU-MR number 3 (Meteor-M N2-3), primary set, and the calibration views.
        # Two packets are cut short: one before the number, one before the views.
        tlm = meteor_time(strip) + bytes(12) + bytes((3 << 4, 0)) + bytes(21) + pack10([64, 960] * 6) + bytes(4)
        vc.add(ccsds_packet(70, seq, tlm[: {3: 18, 5: 30}.get(strip, len(tlm))]))
        seq += 1
    out = vc.frames()
    assert all(len(c) == 1024 for c in out)
    return b"".join(out)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    os.makedirs(sys.argv[1], exist_ok=True)
    ch = channels()
    for name, data in (("metop.cadu", metop_cadus(ch)), ("noaa.raw16", noaa_frames(ch)), ("meteor.cadu", meteor_cadus())):
        with open(os.path.join(sys.argv[1], name), "wb") as f:
            f.write(data)


if __name__ == "__main__":
    main()
