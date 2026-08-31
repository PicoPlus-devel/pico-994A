#!/usr/bin/env python3
"""
Make a TI-99/4A cassette tape (WAV) from a program file.

    tools/mktape.py PROGRAM.FIAD              -> PROGRAM.wav
    tools/mktape.py -o game.wav game.tifiles
    tools/mktape.py --raw image.bin -o out.wav

Takes a V9T9 FIAD file, a TIFILES file, or a raw program image, and writes what the
console would have written had you typed SAVE CS1. Load it back with OLD CS1.

The format was not guessed at: it was read off tapes the hardware wrote, checked against
the matching FIAD files byte for byte. A tape is

    768 x >00                 leader, long enough for the reader to find its level
    >FF                       end of leader
    N, N                      number of 64-byte records, written twice
    then 2N times:
        8 x >00               gap
        >FF                   start of record
        64 data bytes
        1 checksum byte       sum of the 64 bytes, low 8 bits

with each record written out twice in succession so the reader has a second chance at it.
The bits are MSB first, encoded bi-phase mark: the signal flips at every cell boundary,
and once more in the middle of the cell for a 1. A cell is 2304 CPU cycles - 768us at the
console's 3MHz - which was measured off a tape written by a real machine, not taken from
the ~1379 baud figure usually quoted, which is 6% out.
"""
import argparse, struct, sys, os

CPU_HZ      = 3_000_000
CELL_CYCLES = 2304                      # measured; 768us, about 1302 baud
LEADER      = 768                       # bytes of >00 before the tape starts
GAP         = 8                         # bytes of >00 between records
RECORD      = 64

LEVEL_HIGH  = 0xC0                      # ~50% of full scale, as the emulator records
LEVEL_LOW   = 0x40


def program_image(data, force_raw=False):
    """Pull the program out of a FIAD or TIFILES wrapper, or take it as it comes."""
    if force_raw:
        return data, "raw image"

    # TIFILES: a 128-byte header starting >07 "TIFILES".
    if len(data) > 128 and data[0] == 0x07 and data[1:8] == b"TIFILES":
        sectors = struct.unpack(">H", data[8:10])[0]
        eof = data[12]
        return _trim(data[128:], sectors, eof), "TIFILES"

    # V9T9 FIAD: a 128-byte header, filename padded to 10 bytes, then flags and sizes.
    if len(data) > 128 and (len(data) - 128) % 256 == 0:
        flags = data[12]
        sectors = struct.unpack(">H", data[14:16])[0]
        eof = data[16]
        if sectors and sectors * 256 == len(data) - 128:
            kind = "FIAD"
            if not flags & 0x01:
                print(f"warning: {kind} header says this is not a PROGRAM file "
                      f"(flags >{flags:02X}) - saving it anyway", file=sys.stderr)
            return _trim(data[128:], sectors, eof), kind

    return data, "raw image"


def _trim(payload, sectors, eof):
    """Sector count and end-of-file offset give the real length; the rest is padding."""
    length = (sectors - 1) * 256 + (eof if eof else 256)
    return payload[:length]


def build_tape(image):
    """The byte stream on the tape, leader included."""
    if len(image) % RECORD:
        image = image + bytes(RECORD - len(image) % RECORD)
    records = len(image) // RECORD
    if not 1 <= records <= 255:
        sys.exit(f"program is {len(image)} bytes = {records} records; a tape holds 1 to 255")

    out = bytearray(b"\x00" * LEADER)
    out += bytes([0xFF, records, records])
    for r in range(records):
        block = image[r * RECORD:(r + 1) * RECORD]
        checksum = sum(block) & 0xFF
        for _copy in range(2):                      # every record goes down twice
            out += b"\x00" * GAP
            out += b"\xff" + block + bytes([checksum])
    return bytes(out), records


def modulate(data, rate):
    """Bi-phase mark the byte stream into 8-bit unsigned PCM.

    The signal flips at the start of every bit cell, and again halfway through when the
    bit is a 1 - so a 1 is two short pulses and a 0 is one long one. Edge positions are
    kept as floats because a cell is not a whole number of samples (33.87 at 44.1kHz),
    and rounding each one separately would let the error accumulate across the tape.
    """
    cell = rate * CELL_CYCLES / CPU_HZ
    pcm = bytearray()
    level = 0
    pos = 0.0                                       # samples written so far
    t = 0.0                                         # start of the current cell

    def run_to(target):
        nonlocal pos
        while pos < target:
            pcm.append(LEVEL_HIGH if level else LEVEL_LOW)
            pos += 1.0

    for byte in data:
        for i in range(7, -1, -1):                  # MSB first
            level ^= 1                              # edge at the cell boundary
            if (byte >> i) & 1:
                run_to(t + cell / 2)
                level ^= 1                          # and another mid-cell, for a 1
            run_to(t + cell)
            t += cell

    return bytes(pcm)


def wav(pcm, rate):
    n = len(pcm)
    return (b"RIFF" + struct.pack("<I", 36 + n) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 1, rate, rate, 1, 8) +
            b"data" + struct.pack("<I", n) + pcm)


def main():
    ap = argparse.ArgumentParser(description="Make a TI-99/4A cassette WAV from a program file.")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="default: the input name with .wav")
    ap.add_argument("--raw", action="store_true", help="input is a bare program image, no header")
    ap.add_argument("-r", "--rate", type=int, default=44100, help="sample rate (default 44100)")
    args = ap.parse_args()

    data = open(args.input, "rb").read()
    image, kind = program_image(data, args.raw)
    tape, records = build_tape(image)
    pcm = modulate(tape, args.rate)

    out = args.output or os.path.splitext(args.input)[0] + ".wav"
    with open(out, "wb") as f:
        f.write(wav(pcm, args.rate))

    print(f"{out}")
    print(f"  from {args.input} ({kind}), {len(image)} byte image, {records} records")
    print(f"  {len(tape)} bytes on tape, {len(pcm) / args.rate:.1f}s at {args.rate} Hz")


if __name__ == "__main__":
    sys.exit(main())
