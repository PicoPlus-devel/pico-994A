#!/usr/bin/env python3
"""
Inspect a TI-99/4A cassette WAV.

Says what is actually on the tape: how many transitions, how long they are, what bit
rate that implies, and whether it decodes as the bi-phase mark encoding the console
uses. Point it at a tape pico-994A wrote, or at a dump of a real cassette, and compare.

    tools/tapeinfo.py /path/to/FRANK.WAV
    tools/tapeinfo.py -v FRANK.WAV       # also dump the decoded bytes

A healthy TI tape opens with a long run of >00 (the 768-byte leader), then a >FF marker,
then 64-byte records written twice over. If the intervals cluster into two groups about
2:1 apart, the encoding assumption holds and the shorter one is the half cell.
"""
import argparse, struct, sys
from collections import Counter


def read_wav(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[0:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")

    fmt = payload = None
    p = 12
    while p + 8 <= len(data):
        cid = data[p:p + 4]
        sz = struct.unpack("<I", data[p + 4:p + 8])[0]
        body = data[p + 8:p + 8 + sz]
        if cid == b"fmt ":
            fmt = body
        elif cid == b"data":
            payload = body
            break
        p += 8 + sz + (sz & 1)

    if fmt is None or payload is None:
        sys.exit(f"{path}: no fmt/data chunk")

    tag, channels, rate, _, _, bits = struct.unpack("<HHIIHH", fmt[:16])
    # WAVE_FORMAT_EXTENSIBLE keeps the real tag in its SubFormat GUID.
    if tag == 0xFFFE and len(fmt) >= 26:
        tag = struct.unpack_from("<H", fmt, 24)[0]

    step = channels * (bits // 8)
    rng = range(0, len(payload) - step + 1, step)

    if tag == 3 and bits == 32:            # IEEE float, what most real dumps are
        samples = [int(max(-1.0, min(1.0, struct.unpack_from("<f", payload, i)[0])) * 32767)
                   for i in rng]
    elif tag != 1:
        sys.exit(f"{path}: unsupported format {tag}")
    elif bits == 8:
        samples = [(payload[i] - 128) << 8 for i in rng]
    elif bits == 16:
        samples = [struct.unpack_from("<h", payload, i)[0] for i in rng]
    elif bits == 24:
        samples = [struct.unpack_from("<i", payload[i:i + 3] + (b"\xff" if payload[i + 2] & 0x80 else b"\x00"))[0] >> 8
                   for i in rng]
    elif bits == 32:
        samples = [struct.unpack_from("<i", payload, i)[0] >> 16 for i in rng]
    else:
        sys.exit(f"{path}: {bits}-bit is not supported")

    return rate, channels, bits, samples


def find_edges(samples):
    """Same shape of detector the emulator uses: adaptive baseline plus hysteresis."""
    if not samples:
        return []
    prime = samples[:64]
    dc = sum(prime) / len(prime)
    env = 0.0
    level = None
    edges = []
    for i, s in enumerate(samples):
        dc += (s - dc) / 1024.0
        ac = s - dc
        mag = abs(ac)
        env += (mag - env) / (8.0 if mag > env else 2048.0)
        hyst = max(env / 8.0, 64.0)
        if level is None:
            level = 1 if ac > 0 else 0
        elif level and ac < -hyst:
            level = 0
            edges.append(i)
        elif not level and ac > hyst:
            level = 1
            edges.append(i)
    return edges


def classify(intervals):
    """Split the intervals into a short and a long group and report the boundary.

    Two-means rather than anything cleverer. A real cassette does not produce two
    discrete interval lengths: tape speed wanders, so each group is a spread, and
    picking the widest gap between observed lengths lands somewhere meaningless.
    """
    if len(intervals) < 8:
        return None

    # Seed from the trimmed extremes, not the quartiles: on a tape that is mostly leader
    # the short cells are a small minority and both quartiles land in the same group.
    ordered = sorted(intervals)
    lo = ordered[len(ordered) // 100]
    hi = ordered[-1 - len(ordered) // 100]
    if hi < lo * 1.3:                       # one cluster only
        return None

    for _ in range(30):
        split = (lo + hi) / 2
        a = [v for v in intervals if v < split]
        b = [v for v in intervals if v >= split]
        if not a or not b:
            return None
        na, nb = sum(a) / len(a), sum(b) / len(b)
        if abs(na - lo) < 0.01 and abs(nb - hi) < 0.01:
            break
        lo, hi = na, nb

    return (lo + hi) / 2


def decode(edges, split):
    """Bi-phase mark: a short pair is a 1, a long interval is a 0."""
    bits, pending = [], False
    for a, b in zip(edges, edges[1:]):
        if (b - a) < split:
            if pending:
                bits.append(1)
                pending = False
            else:
                pending = True
        else:
            bits.append(0)
            pending = False
    return bits


def to_bytes(bits):
    return bytes(int("".join(str(b) for b in bits[i:i + 8]), 2)
                 for i in range(0, len(bits) - 7, 8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("wav")
    ap.add_argument("-v", "--verbose", action="store_true", help="dump decoded bytes")
    args = ap.parse_args()

    rate, channels, bits, samples = read_wav(args.wav)
    dur = len(samples) / rate if rate else 0
    print(f"{args.wav}")
    print(f"  {rate} Hz, {bits}-bit, {'stereo' if channels == 2 else 'mono'}, "
          f"{len(samples)} frames ({dur:.2f}s)")

    edges = find_edges(samples)
    print(f"  {len(edges)} transitions")
    if len(edges) < 2:
        print("  -- nothing on this tape: the data line never moved --")
        return 1

    iv = [b - a for a, b in zip(edges, edges[1:])]
    us = lambda n: n * 1e6 / rate
    print(f"  intervals: min {us(min(iv)):.0f}us  max {us(max(iv)):.0f}us")

    top = Counter(iv).most_common(6)
    print("  most common: " + ", ".join(f"{us(v):.0f}us x{n}" for v, n in top))

    split = classify(iv)
    if split is None:
        print("  -- only one interval length: this is a tone, not data --")
        return 1

    short = [v for v in iv if v < split]
    long_ = [v for v in iv if v >= split]
    sa = sum(short) / len(short) if short else 0
    la = sum(long_) / len(long_) if long_ else 0
    print(f"  short: {len(short)} averaging {us(sa):.1f}us")
    print(f"  long:  {len(long_)} averaging {us(la):.1f}us")
    if sa and la:
        print(f"  long/short ratio {la / sa:.2f}  (bi-phase mark wants 2.00)")

    cell = la if la else sa * 2
    if cell:
        print(f"  => cell {us(cell):.1f}us, {rate / cell:.1f} baud, "
              f"{3_000_000 / (rate / cell):.0f} CPU cycles per cell")

    bits = decode(edges, split)
    data = to_bytes(bits)
    print(f"  decoded {len(bits)} bits = {len(data)} bytes")

    # Work in bits, not bytes. The recovered stream starts a few bits into the leader -
    # the detector needs a moment to find the baseline - so byte boundaries are shifted by
    # an unknown amount and a byte-aligned search for >FF finds it in the wrong place, or
    # not at all. A run of zeros reads the same at any alignment; the marker does not.
    lead = 0
    while lead < len(bits) and bits[lead] == 0:
        lead += 1
    print(f"  leader: {lead} zero bits = {lead / 8:.1f} bytes  (a real leader is 768)")

    if lead < len(bits):
        run = 0
        while lead + run < len(bits) and bits[lead + run] == 1:
            run += 1
        print(f"  then {run} one bits" + ("  (>FF marker)" if run == 8 else "  -- expected 8 --"))

    if args.verbose:
        for i in range(0, min(len(data), 512), 16):
            print("   ", " ".join(f"{b:02X}" for b in data[i:i + 16]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
