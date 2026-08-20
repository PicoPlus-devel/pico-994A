# pico-994A

## Introduction

**pico-994A** is a Texas Instruments TI-99/4A emulator for RP2350-based microcontrollers.
It is based on the [DS994a](https://github.com/wavemotion-dave/DS994a) emulation core by
Dave Bernazzani (wavemotion-dave), integrated with the video, audio, menu, and SD card
framework from [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus).

> [!IMPORTANT]
> RP2350 only (Pico 2 and variants). The RP2040 is not supported - the TI-99/4A needs
> ~160 KB of emulated memory on top of the framework's 150 KB framebuffer.

This project is part of a family of Raspberry Pi Pico emulator projects:

- NES: [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus)
- Sega Master System / Game Gear: [pico-smsplus](https://github.com/fhoedemakers/pico-smsplus)
- Game Boy / Game Boy Color: [pico-peanutGB](https://github.com/fhoedemakers/pico-peanutGB)
- Sega Mega Drive / Genesis: [pico-genesisPlus](https://github.com/fhoedemakers/pico-genesisPlus)
- PC Engine / TurboGrafx-16: [pico-pcePlus](https://github.com/fhoedemakers/pico-pcePlus)
- Odyssey 2 / VideoPac: [pico-pacPlus](https://github.com/fhoedemakers/pico-pacPlus)
- Super Nintendo: [pico_snesPlus](https://github.com/fhoedemakers/pico_snesPlus)
- Multi-emulator bundle for the [Adafruit Fruit Jam](https://www.adafruit.com/product/6200): [retroJam](https://github.com/fhoedemakers/retroJam)

***

## What it emulates

- **TMS9900 CPU, TMS9918A video and TMS9919/SN94624 sound** - the full console.
- **32K Memory Expansion** - always fitted, no configuration needed.
- **Full keyboard** - a USB keyboard is mapped onto the TI keyboard matrix. All 48 TI
  keys are reachable, and PC keys the TI does not have (arrows, Backspace, Delete,
  Escape, F1-F10) are translated into the FCTN combinations a TI user would type.
- **Two joystick ports** - USB gamepads, plus GPIO NES/SNES pads and Wii controllers.
- **Cartridges from SD card** - `.rpk` Rom PacKs and the classic C/D/G/8/9/0 `.bin` sets.
- **TI BASIC** - built into the console GROMs; see [TI BASIC](#ti-basic) below.
- **Disk controller** - DSK1, DSK2 and DSK3 against `.DSK` sector images, read and write.
- **SAMS memory expansion** - 1 MB to 8 MB on boards with PSRAM fitted.
- **Speech Synthesizer** - real LPC synthesis of the TMS5200, not sampled playback.
  See [Speech](#speech) below.

Not emulated yet: cassette (CS1/CS2), save states, and the p-code card.
See [Not yet done](#not-yet-done).

***

## Setup Overview

1. Prepare an SD card formatted as FAT32 (recommended) or exFAT.
2. Put the console BIOS files in `/bios/` on the card - see [BIOS files](#bios-files).
   **Nothing runs without them.**
3. Transfer cartridges to the card, preferably in `/roms/TI99` (subdirectories work).
4. Insert the SD card and use the menu to browse, select, and play.

## BIOS files

| File | Size | Required | What it is |
|------|------|----------|------------|
| `994aROM.bin`  | 8 KB  | **yes** | Console ROM |
| `994aGROM.bin` | 24 KB | **yes** | Console GROMs - TI BASIC and the master title screen live here |
| `994aDISK.bin` | 8 KB  | no      | TI Disk Controller DSR - needed for DSK1/2/3 |
| `spchrom.bin`  | 32 KB | no      | Speech Synthesizer resident vocabulary - see [Speech](#speech) |

`/bios/` is searched first, then `/roms/bios/` and `/roms/ti99/`, so an SD card already
prepared for DS994a works unchanged.

These are copyrighted TI system software and are not distributed here.

## Cartridges

**`.rpk` is the format to prefer.** A Rom PacK is a single zip holding every part of the
cartridge plus a `layout.xml` describing how they map, so one file is all you need.

The classic multi-file `.bin` sets also work. Select any part in the menu and the rest
are picked up automatically from the same folder:

| Suffix | Contents |
|--------|----------|
| `xxxC.bin` | CPU ROM at `>6000` |
| `xxxD.bin` | Second 8 KB bank |
| `xxxG.bin` | GROM at `>6000` (up to 40 KB) |
| `xxx8.bin` | Multi-bank image, non-inverted |
| `xxx9.bin` | Multi-bank image, inverted |
| `xxx0.bin` | Replaces the console GROMs |

Cartridge ROM up to 64 KB is held in SRAM; anything larger needs a board with PSRAM.

## Making `.rpk` files

`tools/mkrpk.py` packs the `.bin` sets into Rom PacKs, working out the PCB type from the
naming convention and the file sizes. It needs nothing but Python 3, and the layout it
writes is the one MAME reads.

Point it at a single part and it collects the rest of that set:

```
$ tools/mkrpk.py PARSECC.bin
PARSEC.rpk  pcb=standard  rom=PARSECC.bin 8K grom=PARSECG.bin 24K
```

Or convert a whole folder at once, one `.rpk` per cartridge:

```
$ ls roms
ALPINERC.bin  ALPINERG.bin  MMC.bin  MMG.bin  MUNCHMNC.bin  MUNCHMNG.bin
XBC.bin  XBD.bin  XBG.bin

$ tools/mkrpk.py -d rpk roms
rpk/ALPINER.rpk  pcb=standard  rom=ALPINERC.bin 8K grom=ALPINERG.bin 24K
rpk/MM.rpk  pcb=standard  rom=MMC.bin 8K grom=MMG.bin 12K
rpk/MUNCHMN.rpk  pcb=standard  rom=MUNCHMNC.bin 8K grom=MUNCHMNG.bin 18K
rpk/XB.rpk  pcb=paged12k  rom=XBC.bin 4K rom2=XBD.bin 8K grom=XBG.bin 30K
```

Extended BASIC came out as `paged12k` because its ROM is 4 KB with a second 8 KB bank
behind it. `-v` shows the layout that went into the archive:

```
$ tools/mkrpk.py -v -f -d rpk roms/XBC.bin
rpk/XB.rpk  pcb=paged12k  rom=XBC.bin 4K rom2=XBD.bin 8K grom=XBG.bin 30K
    | <?xml version="1.0" encoding="utf-8"?>
    | <romset listname="xb">
    |     <resources>
    |         <rom id="romimage" file="XBC.bin"/>
    |         <rom id="romimage2" file="XBD.bin"/>
    |         <rom id="gromimage" file="XBG.bin"/>
    |     </resources>
    |     <configuration>
    |         <pcb type="paged12k">
    |             <socket id="rom_socket" uses="romimage"/>
    |             <socket id="rom2_socket" uses="romimage2"/>
    |             <socket id="grom_socket" uses="gromimage"/>
    |         </pcb>
    |     </configuration>
    | </romset>
```

The filenames cannot say everything. Mini Memory is an ordinary ROM + GROM set on disk,
so it converted as `standard`, but the real cartridge carries 4 KB of battery-backed RAM.
`--pcb` sets what the files cannot:

```
$ tools/mkrpk.py --pcb minimem --listname minimem -f -o rpk/MM.rpk roms/MMC.bin
rpk/MM.rpk  pcb=minimem  rom=MMC.bin 8K grom=MMG.bin 12K
```

`--list-pcb` prints every type the loader understands - `mbx`, `super`, `pagedcru`,
`paged7` and the rest. `--rom`, `--rom2` and `--grom` fill the sockets directly when the
files are not named to the convention, and `-n` says what would be built without writing
anything.

## TI BASIC

TI BASIC is part of the console's own GROMs, so it needs no cartridge - just
`994aGROM.bin` in `/bios/`. Since the menu is built around picking a file, a small
marker file gives it an entry like any other title:

1. Copy `assets/TI BASIC.tib` to `/roms/TI99` on the SD card.
   (pico-994A also creates it for you on first run if the folder exists.)
2. Select it in the menu.
3. Press any key at the master title screen, then **1** for TI BASIC.

Any `.tib` file works the same way - the name is yours to choose, the content is ignored.

## Disks

With `994aDISK.bin` present, DSK1-3 answer as a standard TI Disk Controller against
360 KB `.DSK` sector images (v9t9 format).

Disks sitting next to a cartridge are mounted automatically: for `GAME.bin` the emulator
looks for `GAME1.dsk`, `GAME2.dsk` and `GAME3.dsk`.

From TI BASIC this gives you somewhere to keep your programs:

```
SAVE DSK1.MYPROG
OLD DSK1.MYPROG
```

Sector writes go straight to the card rather than being buffered and written back on
exit, so pulling the power will not lose a program you just saved.

## Speech

The Solid State Speech Synthesizer is emulated properly - the TMS5200's LPC-10 vocal
tract model, not recorded samples. A ten-stage lattice filter is driven by a
pitch-controlled chirp for voiced sounds and by noise for unvoiced ones, with the
parameters interpolated across eight periods of every 25 ms frame, exactly as the chip
does it. It costs well under 1% of the CPU.

**The module is always reported as attached**, and most speech needs nothing extra:
cartridges stream their own LPC data with the Speak External command, so Parsec, Alpiner,
Moonmine, Star Trek and the rest talk with no additional files at all.

`spchrom.bin` in `/bios/` adds the ~32 KB resident vocabulary held in the module's two
TMS6100 ROMs. That is what TI Extended BASIC's `CALL SAY` and the Terminal Emulator II
read from. Without it, those fall silent while cartridge speech keeps working. The file
is only loaded when present, so it costs nothing if you leave it out.

Early TI-99/4A modules used the **TMS5200** (TMC0285 / CD2501E) and later ones the
TMS5220; their pitch and reflection-coefficient tables differ audibly. The port defaults
to the TMS5200, which is what the machine shipped with. `SpeechSetChip()` selects the
other set.

## Keyboard

A USB keyboard maps onto the TI keyboard directly. Letters, digits, `.` `,` `/` `;` `=`,
Enter and Space are where you expect, and the shifted number row matches the TI's
(`!@#$%^&*()`).

| PC key | TI equivalent |
|--------|---------------|
| Left Alt / Right Alt | **FCTN** |
| Ctrl | **CTRL** |
| Shift | **SHIFT** |
| Caps Lock | **ALPHA LOCK** (latching, as on the real machine) |
| Arrow keys | FCTN + E / S / D / X |
| Backspace | FCTN + S |
| Delete / Insert | FCTN + 1 / FCTN + 2 |
| Escape | FCTN + 9 (BACK) |
| F1 - F9 | FCTN + 1 - 9 (DEL, INS, ERASE, CLEAR, BEGIN, PROC'D, AID, REDO, BACK) |
| F10 | FCTN + = (QUIT) |
| `-` | SHIFT + `/` |
| `'` `[` `]` `\` | FCTN + O / R / T / Z |

Anything else is reachable by holding Alt (FCTN) or Shift and pressing the TI key, just
as on the original keyboard.

## Controls

| Input | Action |
|-------|--------|
| D-pad / stick | Joystick 1 (player 2's pad drives Joystick 2) |
| A or B | Fire |
| SELECT + START | Settings menu |
| SELECT + UP/DOWN | Screen mode |
| START + A | Toggle FPS display |
| SELECT + START + UP + A | Reboot into BOOTSEL mode |

***

## Supported hardware

Built for every RP2350 configuration `pico_shared` supports:

| HW_CONFIG | Board |
|-----------|-------|
| 1 | Pimoroni Pico DV Demo Base |
| 2 | Adafruit DVI + microSD breakouts / custom PCB |
| 5 | Adafruit Metro RP2350 |
| 6 | Waveshare RP2350-Zero with custom PCB |
| 7 | Waveshare RP2350-PiZero |
| 8 | Adafruit Fruit Jam (default) |
| 9 | Waveshare RP2350-USB-A |
| 10 | Spotpear HDMI board |
| 12 | Murmulator M1 |
| 13 | Murmulator M2 |
| 14 | Adafruit Feather RP2350 |

HW_CONFIG 3 and 4 are RP2040-only boards; 11 is a deprecated pinout.

## Building

```
export PICO_SDK_PATH=/path/to/pico-sdk
export PICO_PIO_USB_PATH=/path/to/Pico-PIO-USB
git clone --recursive https://github.com/fhoedemakers/pico-994A.git
cd pico-994A
./bld.sh -c 8 -2          # one config
./buildAll.sh             # everything, into releases/
```

`./bld.sh -h` lists all options.

***

## Known issues

- **No speech in Parsec.** The synthesiser passes its own test harness, so the fault is
  most likely in how the chip is driven rather than in the LPC code. Under investigation.

## Not yet done

- **Cassette (CS1/CS2).** Feasible - the console DSR bit-bangs the TMS9901 (CRU bits
  22/23 motor, 24 audio gate, 25 out, 27 in) at ~1379 baud, and the 9901 timer it times
  against is already emulated. It is new code rather than a port; DS994a does not
  implement it either. Use a disk for now.
- **Save states.**
- **p-code card.**

***

## Credits

- **DS994a** by [wavemotion-dave](https://github.com/wavemotion-dave/DS994a) - the
  TI-99/4A emulation core this port is built on.
- **TMS5200 speech** implemented from TI's own patents (US 4,209,804 for the lattice
  filter, 4,331,836 for the chirp excitation, 4,335,277 for the frame and command
  logic); the coefficient tables are the chip's published data.
- **Classic99** by Mike Brent (Tursi) - TMS9900 CPU and disk controller scaffolding.
- **ColEM** by Marat Fayzullin - TMS9918A video driver.
- **SN76496 PSG** - C core from pico-smsplus (SMS Plus lineage, Charles MacDonald).
- **lowzip** and **yxml** - MIT, used for `.rpk` loading.
- **PicoDVI** by [@shuichi_takano](https://github.com/shuichi-takano) and the HSTX HDMI
  driver by fliperama86.
- NES/Wii controller support from [@PaintYourDragon](https://github.com/PaintYourDragon)
  and Adafruit.

See [LICENSE](LICENSE) for the full terms - in particular, the emulation core is used
under a **non-commercial** licence, which is stricter than the GPL cores in the sibling
emulators.
