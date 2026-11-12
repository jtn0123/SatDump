#!/usr/bin/env python3
"""A short made-up MetOp AHRPT signal for tests/cli/supervision.sh. Nothing here is received data.

  ahrpt_baseband.py <output .cs8 file> <frames, a multiple of 3>

QPSK at 2 333 333 symbols/s, sampled at 6 MS/s as 8-bit I/Q (cs8), carrying CCSDS frames (CADUs:
the sync marker 1ACFFC1D, then 1020 bytes of fixed pseudo-random fill). The bits go through the
K=7 convolutional code (171 and 133 octal), punctured to rate 3/4 the way SatDump's MetOp decoder
puts it back: for each three bits the two symbols are (a0, b0) and (a2, b1). The pulses are
square, with no noise and no carrier offset, so the demodulator locks at once. The file source
plays the file in a loop; with a whole number of frames (a multiple of 3, so the puncturing
lines up) the decoder stays locked, or locks again within a frame or two where the loop restarts.

Only the standard library is used, and the file is byte-for-byte the same on every machine.
"""

import random
import sys

# 171 and 133 octal, read with the newest bit first; here the newest bit is the lowest one
G1, G2 = 0b1001111, 0b1101101
AMP = 90  # of 127


def parity(x):
    return bin(x).count("1") & 1


def frames(count):
    rng = random.Random(5500)
    out = bytearray()
    for _ in range(count):
        out += b"\x1a\xcf\xfc\x1d" + bytes(rng.randrange(256) for _ in range(1020))
    return bytes(out)


def encode(data):
    """The two code outputs (a for 171, b for 133) as strings of '0' and '1', a byte at a time."""
    # For each state (the last six bits) and byte: the 8 bits of a, of b, and the next state
    table = {}
    for state in range(64):
        for byte in range(256):
            reg, a, b = state, [], []
            for i in range(7, -1, -1):
                reg = ((reg << 1) | ((byte >> i) & 1)) & 0x7F
                a.append("1" if parity(reg & G1) else "0")
                b.append("1" if parity(reg & G2) else "0")
            table[state, byte] = ("".join(a), "".join(b), reg & 0x3F)
    state, a_out, b_out = 0, [], []
    for byte in data:
        a, b, state = table[state, byte]
        a_out.append(a)
        b_out.append(b)
    return "".join(a_out), "".join(b_out)


def interleave(x, y):
    return "".join(map("".join, zip(x, y)))


def main():
    if len(sys.argv) != 3 or int(sys.argv[2]) % 3 != 0:
        sys.exit("usage: ahrpt_baseband.py <output .cs8 file> <frames, a multiple of 3>")
    a, b = encode(frames(int(sys.argv[2])))
    i_bits = interleave(a[0::3], a[2::3])
    q_bits = interleave(b[0::3], b[1::3])
    # 6 MS/s is 18/7 samples per symbol: 2 or 3 samples each, the same 7-symbol pattern over again
    counts = [((k + 1) * 18) // 7 - (k * 18) // 7 for k in range(7)]
    level = {"1": AMP, "0": 256 - AMP}
    sample = {(i, q): bytes((level[i], level[q])) for i in "01" for q in "01"}
    out = b"".join(sample[iq] * counts[k % 7] for k, iq in enumerate(zip(i_bits, q_bits)))
    with open(sys.argv[1], "wb") as f:
        f.write(out)


if __name__ == "__main__":
    main()
