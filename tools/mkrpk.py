#!/usr/bin/env python3
# =====================================================================================
# mkrpk.py - build .rpk cartridges for pico-994A
#
# An .rpk (Rom PacK) is a zip holding a cartridge's ROM/GROM images plus a layout.xml
# saying which image goes in which socket and how the banking works. One file per
# cartridge, which is why it is the format to prefer on the SD card.
#
# This builds RPKs from the classic C/D/G/8/9 .bin sets, picking the PCB type from the
# naming convention and the file sizes:
#
#   mkrpk.py PARSECC.bin                   one set -> PARSEC.rpk (picks up PARSECG.bin)
#   mkrpk.py -d out roms/                  every set found in a folder
#   mkrpk.py --pcb minimem --rom MMC.bin --grom MMG.bin -o "Mini Memory.rpk"
#
# The archive is written as plain zip - deflate, no zip64, no data descriptors, sizes in
# the local headers - which is what the lowzip reader in ti99/rpk/ reads. Timestamps are
# fixed so the same inputs always produce the same file.
#
# Also usable with MAME/MESS, which reads the same layout.
# =====================================================================================

import argparse
import sys
import zipfile
from pathlib import Path

# Part characters of the classic multi-file sets - see the Cartridges table in README.md
PART_CHARS = "CDG8930"
CART_EXTS = (".bin", ".rom")

BANK = 0x2000       # 8K, one cartridge bank
MAX_GROM = 0xA000   # 40K of cartridge GROM at GROM >6000

# yxml in rpk.c parses attribute values into a 64 byte buffer, and rom_file[] is 64 too
MAX_ATTR = 62
# layout.xml is extracted into fileBuf[] with a 4096 byte cap and must stay NUL-terminated
MAX_LAYOUT = 4000

# The PCB types rpk_load() understands. MAME has more, but these are what gets loaded.
PCB_TYPES = {
    "standard":  "8K ROM at >6000 plus optional GROM - most first-party TI carts",
    "paged":     "8K ROM plus a second 8K bank at >6000, plus optional GROM",
    "paged16k":  "same as paged",
    "paged12k":  "4K ROM plus an 8K second bank - the real Extended BASIC dump",
    "gromemu":   "GROM emulation - loaded exactly like standard here",
    "paged377":  "flat multi-bank ROM, non-inverted",
    "paged378":  "flat multi-bank ROM, non-inverted - the usual homebrew/FinalGROM shape",
    "paged379i": "flat multi-bank ROM, inverted banking",
    "mbx":       "standard load plus 1K of MBX RAM and its banking hotspots",
    "minimem":   "standard load plus 4K of RAM at >7000",
    "pagedcru":  "CRU-paged ROM - DataBiotics carts",
    "super":     "standard load plus 32K of CRU-paged RAM - Super Cart",
    "paged7":    "paged ROM at >7000 - TI-Calc",
}

# Types that take a second ROM in rom2_socket, and the RAM each RAM-carrying type holds
PCB_WITH_ROM2 = ("paged", "paged16k", "paged12k", "paged7")
PCB_RAM_SIZE = {"minimem": 0x1000, "super": 0x8000, "mbx": 0x400}
# rpk_load_paged378() reaches its GROM check from inside the rom_socket branch, so a GROM
# in a 377/378 layout is never loaded. rpk_load_paged379i() has no GROM handling at all.
PCB_NO_GROM = ("paged377", "paged378", "paged379i")

warnings = 0


def warn(msg):
    global warnings
    warnings += 1
    print("mkrpk: warning: %s" % msg, file=sys.stderr)


def die(msg):
    print("mkrpk: error: %s" % msg, file=sys.stderr)
    sys.exit(1)


def human(size):
    if size >= 1024 and size % 1024 == 0:
        return "%dK" % (size // 1024)
    return "%d bytes" % size


def banks(size):
    return size // BANK + (1 if size % BANK else 0)


# MAME list names are lowercase alphanumerics, and rpk.c matches a handful of them
# ("qbert", "frogger") to set up a cart's controls - so keep the default in that shape.
LISTNAME_CHARS = "abcdefghijklmnopqrstuvwxyz0123456789_"


def slug(name):
    """'Mini Memory' -> 'minimemory'. Falls back to 'cart' if nothing is left."""
    return "".join(c for c in name.lower() if c in LISTNAME_CHARS) or "cart"


# -------------------------------------------------------------------------------------
# Working out what a file is. "PARSECC.bin" is the 'C' part of the set "PARSEC";
# "TUNNELS.bin" ends in a character that is not a part letter, so it is a single image.
# -------------------------------------------------------------------------------------
def split_part(path):
    stem = path.stem
    if len(stem) >= 2 and stem[-1].upper() in PART_CHARS:
        return stem[:-1], stem[-1].upper()
    return stem, None


def collect_set(path, siblings=True):
    """Return (base name, {part char: path}). A single image is keyed 'SINGLE'."""
    base, part = split_part(path)
    if part is None:
        return base, {"SINGLE": path}

    parts = {part: path}
    if siblings:
        for sib in sorted(path.parent.iterdir()):
            if sib == path or not sib.is_file():
                continue
            if sib.suffix.lower() != path.suffix.lower():
                continue
            sib_base, sib_part = split_part(sib)
            if sib_part and sib_base.upper() == base.upper():
                parts.setdefault(sib_part, sib)
    return base, parts


def choose_pcb(parts):
    """Pick the PCB type and the socket roles for a collected set."""
    roles = {}

    if "0" in parts:
        warn("%s replaces the console GROMs, which no .rpk socket can do - "
             "keep that one as a .bin set" % parts["0"].name)

    if "SINGLE" in parts:
        rom = parts["SINGLE"]
        roles["rom"] = rom
        return ("paged378" if rom.stat().st_size > BANK else "standard"), roles

    if "8" in parts:
        roles["rom"] = parts["8"]
        pcb = "paged378"
    elif "9" in parts or "3" in parts:
        roles["rom"] = parts.get("9") or parts["3"]
        pcb = "paged379i"
    elif "C" in parts:
        roles["rom"] = parts["C"]
        size = parts["C"].stat().st_size
        if "D" in parts:
            roles["rom2"] = parts["D"]
            pcb = "paged12k" if size <= 0x1000 else "paged"
        elif size > BANK:
            pcb = "paged378"        # a 'C' file holding more than one bank
        else:
            pcb = "standard"
    elif "G" in parts:
        pcb = "standard"            # GROM only, nothing in the ROM sockets
    else:
        return None, roles

    if "G" in parts:
        roles["grom"] = parts["G"]

    return pcb, roles


# -------------------------------------------------------------------------------------
# Checks that catch the layouts rpk_load() cannot make sense of, before writing anything
# -------------------------------------------------------------------------------------
def check(pcb, roles, pad):
    rom = roles.get("rom")
    rom2 = roles.get("rom2")
    grom = roles.get("grom")

    if rom2 and pcb not in PCB_WITH_ROM2:
        warn("pcb '%s' has no rom2_socket - %s will be ignored" % (pcb, rom2.name))
    if grom and pcb in PCB_NO_GROM:
        warn("pcb '%s' does not load a GROM - %s will be ignored. A cart needing both "
             "its banks and a GROM loads correctly as pcb 'standard', which banks by "
             "ROM size just the same" % (pcb, grom.name))
    if not rom and not grom:
        die("nothing to put in a socket")

    if grom and grom.stat().st_size > MAX_GROM:
        warn("%s is %s - only the first 40K of cartridge GROM is mapped"
             % (grom.name, human(grom.stat().st_size)))

    if rom:
        size = rom.stat().st_size
        if pcb == "standard" and size > BANK:
            # rpk_load_standard() banks on its own, which is handy but not what MAME does
            warn("%s is %s in a 'standard' layout - pico-994A banks it, MAME maps only "
                 "the first 8K" % (rom.name, human(size)))
        if pcb == "paged12k" and size > 0x1000:
            warn("pcb 'paged12k' expects a 4K ROM, %s is %s" % (rom.name, human(size)))
        if pcb in ("paged", "paged16k") and size != BANK:
            warn("pcb '%s' expects an 8K first bank, %s is %s" % (pcb, rom.name, human(size)))
        if pcb in ("paged", "paged16k", "paged7") and not rom2:
            warn("pcb '%s' switches in a second bank but no rom2 was given" % pcb)

        count = banks(size)
        if count > 1:
            if size % BANK:
                warn("%s is %s, not a whole number of 8K banks - the last bank runs past "
                     "the end of the image" % (rom.name, human(size)))
            if count & (count - 1) and pcb not in ("paged", "paged16k", "paged12k", "paged7"):
                msg = ("%s holds %d banks - banking masks up to %d, so the missing banks "
                       "read as unwritten memory" % (rom.name, count, 1 << count.bit_length()))
                if not pad:
                    msg += ". --pad rounds the image up"
                warn(msg)
        if size > 64 * 1024:
            warn("%s is %s - cartridge ROM over 64K needs a board with PSRAM"
                 % (rom.name, human(size)))


def padded(path, role, pcb, pad):
    """Read a member, rounding a multi-bank ROM up to a power-of-two bank count.

    Only the rom_socket image is banked, so it is the only one worth padding - GROM
    and the fixed second bank are mapped whole.
    """
    data = path.read_bytes()
    if not pad or role != "rom" or pcb in ("paged", "paged16k", "paged12k", "paged7"):
        return data
    count = banks(len(data))
    if count <= 1:
        return data
    want = 1 << (count - 1).bit_length()
    return data.ljust(want * BANK, b"\xff")


# -------------------------------------------------------------------------------------
# layout.xml - the same shape MAME writes, with the socket ids rpk_load() looks for
# -------------------------------------------------------------------------------------
def build_layout(listname, pcb, names, ram):
    resources = []
    sockets = []
    for role, socket_id, rom_id in (("rom", "rom_socket", "romimage"),
                                    ("rom2", "rom2_socket", "romimage2"),
                                    ("grom", "grom_socket", "gromimage")):
        if role in names:
            resources.append('        <rom id="%s" file="%s"/>' % (rom_id, names[role]))
            sockets.append('            <socket id="%s" uses="%s"/>' % (socket_id, rom_id))

    if ram and pcb in PCB_RAM_SIZE:
        resources.append('        <ram id="ramimage" type="persistent" length="0x%x" '
                         'filename="%s.nv"/>' % (PCB_RAM_SIZE[pcb], listname))
        sockets.append('            <socket id="ram_socket" uses="ramimage"/>')

    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<romset listname="%s">\n'
            '    <resources>\n%s\n    </resources>\n'
            '    <configuration>\n'
            '        <pcb type="%s">\n%s\n        </pcb>\n'
            '    </configuration>\n'
            '</romset>\n' % (listname, "\n".join(resources), pcb, "\n".join(sockets)))


def check_layout(layout, listname, names):
    if len(listname) > MAX_ATTR:
        die("listname '%s' is longer than %d characters" % (listname, MAX_ATTR))
    if not listname.isascii():
        die("listname '%s' is not plain ASCII" % listname)
    for name in names.values():
        if len(name) > MAX_ATTR:
            die("'%s' is longer than %d characters - rename it" % (name, MAX_ATTR))
        if not name.isascii():
            die("'%s' is not plain ASCII - the loader cannot match it" % name)
    if len(layout.encode()) > MAX_LAYOUT:
        die("layout.xml would be %d bytes, over the %d the loader reads"
            % (len(layout.encode()), MAX_LAYOUT))


def write_rpk(out, layout, members, store):
    method = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
    epoch = (1980, 1, 1, 0, 0, 0)   # fixed, so the same inputs give the same file

    def entry(zf, name, data):
        info = zipfile.ZipInfo(name, date_time=epoch)
        info.compress_type = method
        info.external_attr = 0o644 << 16
        zf.writestr(info, data)

    with zipfile.ZipFile(out, "w", allowZip64=False) as zf:
        entry(zf, "layout.xml", layout)     # first member, as MAME writes it
        for name, _role, data in members:
            entry(zf, name, data)


# -------------------------------------------------------------------------------------
# One cartridge: collect, decide, check, write
# -------------------------------------------------------------------------------------
def make_rpk(roles, pcb, out, listname, args):
    check(pcb, roles, args.pad)

    names = {}
    used = set()
    members = []
    for role in ("rom", "rom2", "grom"):
        src = roles.get(role)
        if not src:
            continue
        name = src.name
        while name.lower() in used:                     # same basename from two folders
            name = "%s-%s%s" % (Path(name).stem, role, Path(name).suffix)
        used.add(name.lower())
        names[role] = name
        members.append((name, role, padded(src, role, pcb, args.pad)))

    layout = build_layout(listname, pcb, names, args.ram)
    check_layout(layout, listname, names)

    if out.exists() and not args.force:
        die("%s exists - use -f to overwrite" % out)

    if not args.quiet:
        shown = " ".join("%s=%s %s" % (role, name, human(len(data)))
                         for name, role, data in members)
        print("%s  pcb=%s  %s" % (out, pcb, shown))
    if args.verbose:
        print("".join("    | %s\n" % line for line in layout.splitlines()), end="")

    if not args.dry_run:
        out.parent.mkdir(parents=True, exist_ok=True)
        write_rpk(out, layout, members, args.store)


def scan_dir(folder):
    """Group every cart image in a folder into sets, one entry per cartridge."""
    sets = {}
    for path in sorted(folder.iterdir()):
        if not path.is_file() or path.suffix.lower() not in CART_EXTS:
            continue
        base, part = split_part(path)
        key = (base.upper(), path.suffix.lower()) if part else (path.stem.upper(), "single")
        sets.setdefault(key, (base, {}))[1].setdefault(part or "SINGLE", path)
    return [value for _, value in sorted(sets.items())]


def main():
    ap = argparse.ArgumentParser(
        description="Build .rpk cartridges for pico-994A from C/D/G/8/9 .bin sets.",
        epilog="With no --pcb the type is picked from the file naming and sizes. "
               "Use --list-pcb to see the types the loader understands.")
    ap.add_argument("inputs", nargs="*", type=Path,
                    help="cart images or folders of them - any part of a set will do")
    ap.add_argument("--rom", type=Path, help="image for rom_socket (>6000)")
    ap.add_argument("--rom2", type=Path, help="image for rom2_socket (second bank)")
    ap.add_argument("--grom", type=Path, help="image for grom_socket (GROM >6000)")
    ap.add_argument("--pcb", choices=sorted(PCB_TYPES), help="force the PCB type")
    ap.add_argument("--listname", help="romset listname - some carts are keyed off it "
                                      "for controller mapping (default: the output "
                                      "name, slugified)")
    ap.add_argument("-o", "--output", type=Path, help="output .rpk (single cartridge)")
    ap.add_argument("-d", "--outdir", type=Path,
                    help="write into this folder (default: next to the source)")
    ap.add_argument("--ram", action="store_true",
                    help="declare the RAM socket for minimem/super/mbx layouts (MAME)")
    ap.add_argument("--pad", action="store_true",
                    help="pad a multi-bank ROM up to a power-of-two bank count with >FF")
    ap.add_argument("--no-siblings", action="store_true",
                    help="use only the files named, do not pick up the rest of a set")
    ap.add_argument("--store", action="store_true", help="store the images uncompressed")
    ap.add_argument("-f", "--force", action="store_true", help="overwrite existing .rpk")
    ap.add_argument("-n", "--dry-run", action="store_true", help="say what would be built")
    ap.add_argument("-v", "--verbose", action="store_true", help="print the layout.xml")
    ap.add_argument("-q", "--quiet", action="store_true", help="only report problems")
    ap.add_argument("--list-pcb", action="store_true", help="list the PCB types and exit")
    args = ap.parse_args()

    if args.list_pcb:
        for pcb, text in PCB_TYPES.items():
            print("  %-10s %s" % (pcb, text))
        return 0

    explicit = args.rom or args.rom2 or args.grom
    if not explicit and not args.inputs:
        ap.error("give a cart image, a folder, or --rom/--grom")
    if explicit and args.inputs:
        ap.error("--rom/--rom2/--grom build one cartridge - drop the positional files")

    # ---------------------------------------------------------------------------
    # Explicit mode: the sockets are spelled out, so only the type is left to pick
    # ---------------------------------------------------------------------------
    if explicit:
        roles = {}
        for role, path in (("rom", args.rom), ("rom2", args.rom2), ("grom", args.grom)):
            if path:
                if not path.is_file():
                    die("%s: no such file" % path)
                roles[role] = path
        pcb = args.pcb
        if not pcb:
            if "rom2" in roles:
                pcb = "paged12k" if roles["rom"].stat().st_size <= 0x1000 else "paged"
            elif "rom" in roles and roles["rom"].stat().st_size > BANK:
                pcb = "paged378"
            else:
                pcb = "standard"

        source = roles.get("rom") or roles["grom"]
        out = args.output
        if not out:
            base, part = split_part(source)
            out = (args.outdir or source.parent) / ("%s.rpk" % (base if part else source.stem))
        elif args.outdir:
            out = args.outdir / out.name
        make_rpk(roles, pcb, out, args.listname or slug(out.stem), args)
        return 0

    # ---------------------------------------------------------------------------
    # Auto mode: each input is one cartridge, each folder is as many as it holds
    # ---------------------------------------------------------------------------
    targets = []
    for path in args.inputs:
        if path.is_dir():
            found = scan_dir(path)
            if not found:
                warn("%s holds no %s files" % (path, "/".join(CART_EXTS)))
            targets.extend((path, base, parts) for base, parts in found)
        elif path.is_file():
            base, parts = collect_set(path, siblings=not args.no_siblings)
            targets.append((path.parent, base, parts))
        else:
            die("%s: no such file or folder" % path)

    if args.output and len(targets) > 1:
        die("-o names one file but %d cartridges were found - use -d" % len(targets))

    for folder, base, parts in targets:
        pcb, roles = choose_pcb(parts)
        if pcb is None:
            warn("%s: nothing loadable in %s" % (base, ", ".join(p.name for p in parts.values())))
            continue
        if args.pcb:
            pcb = args.pcb
        out = args.output or (args.outdir or folder) / ("%s.rpk" % base)
        make_rpk(roles, pcb, out, args.listname or slug(out.stem), args)

    return 0


if __name__ == "__main__":
    sys.exit(main())
