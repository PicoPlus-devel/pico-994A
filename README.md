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
  Needs a board with PSRAM (see [Memory](#memory)).
- **SAMS memory expansion** - 1 MB to 8 MB on boards with PSRAM fitted.
- **Speech Synthesizer** - real LPC synthesis of the TMS5200, not sampled playback.
  See [Speech](#speech) below.
- **Cassette (CS1/CS2)** - `SAVE CS1` and `OLD CS1` against `.wav` or `.cas` files, and
  real cassette dumps load unchanged. See [Cassette](#cassette) below.

Not emulated yet: save states and the p-code card. See [Not yet done](#not-yet-done).

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

## Cassette

The cassette port works, in both directions:

```
SAVE CS1
OLD CS1
```

Tapes live in `/saves/ti99/tapes/` and the deck is in the settings menu (SELECT + START):

| Setting | What it does |
|---------|--------------|
| `Cassette: Empty` | nothing loaded |
| `Cassette: Play <name>` | that tape is in the deck, wound to the start |
| `Cassette: Record WAV` | record a new tape as a `.wav` |
| `Cassette: Record CAS` | record a new tape as a `.cas` |
| `Cassette: Rewind` | wind the loaded tape back (offered while one is playing) |

LEFT/RIGHT pick, `A` commits - nothing is opened or created until you press `A`. Use it to
choose `.cas` instead of `.wav`, to rewind for a second read, or to eject.

You do not have to visit that menu first, though - the deck follows the console:

```
SAVE CS1
* PRESS CASSETTE RECORD CS1
  THEN PRESS ENTER          <- press ENTER, and the emulator asks you to label the tape
* CHECK TAPE (Y OR N)? Y
* REWIND CASSETTE TAPE CS1
  THEN PRESS ENTER          <- just press ENTER; the tape rewinds itself
* PRESS CASSETTE PLAY CS1
  THEN PRESS ENTER          <- and turns itself round to read back
* DATA OK
```

**`SAVE CS1` has no filename.** The TI cassette device takes only a device name: a tape
has no directory, so the console just streams to wherever the tape happens to be sitting,
and nothing in the data carries a name. On a real console you wrote it on the label with a
pen. So the emulator asks for one at the moment the console asks for the tape - which is
the moment you would have been reaching for a cassette anyway. It defaults to `TAPE-01`,
ENTER accepts it, and overwriting an existing tape has to be confirmed. With no USB
keyboard attached the default is used as-is.

`OLD CS1` with an empty deck likewise offers the tapes on the card to pick from, and a
tape already in the deck is wound back at the start of every load - the console has just
asked for that, and on a real deck you would have done it by hand.

The tape is audible while it runs, as it is on a real console - during a load the DSR
opens the audio gate, and during a save the screech is the only sign anything is being
written.

### Tape formats

| | |
|---|---|
| `.wav` | The interoperable one. Any rate, mono or stereo, 8/16/24/32-bit integer or 32-bit float, so **real cassette dumps load unchanged** - they are usually 32-bit float at 48 kHz - and a tape you record opens in Audacity. Written as 44.1 kHz 8-bit mono, about 44 KB per second of tape. |
| `.cas` | The compact one - one bit per tape cell, roughly 170 bytes per second. Carries the bit rate the console actually recorded at in a small header, so playback matches the machine rather than an assumed baud rate. |

Loading runs at real tape speed, because the console is timing the bits as they arrive.
A short BASIC program takes a few seconds; a long one takes as long as it did in 1981.

### Looking inside a tape

`tools/tapeinfo.py` says what is actually on a tape - transitions, interval lengths, the
bit rate they imply, and whether it decodes as the console's encoding. Useful on a tape
pico-994A wrote, and for comparing against a dump of a real cassette:

```
$ tools/tapeinfo.py /media/sdcard/saves/ti99/tapes/FRANK.WAV
  44100 Hz, 8-bit, mono, 246582 frames (5.59s)
  7684 transitions
  short: 2048 averaging 362.2us
  long:  5636 averaging 725.3us
  long/short ratio 2.00  (bi-phase mark wants 2.00)
  => cell 768.0us, 1302.0 baud, 2304 CPU cycles per cell
  leading >00 run: 768 bytes  (a real leader is 768)
  >FF marker at byte 768
```

A healthy tape opens with a long run of `>00` and then a `>FF` marker. `0 transitions`
means the data line never moved and nothing was recorded; one interval length means a
tone rather than data.

### Sample tapes

`assets/tapes/` holds three TI BASIC programs dumped from a real cassette. Copy them to
`/saves/ti99/tapes/` and load one with `OLD CS1` - `13 Bouncing Ball 1.wav` is the
shortest at 13 seconds. See the README in that folder for provenance.

### Where to find more

Dumps of physical cassettes are worth testing against, since they carry the DC offset and
speed variation a tape written by the emulator does not:

- [ftp.whtech.com/Cassettes/](http://ftp.whtech.com/Cassettes/) - the WHTech archive, with
  `Adventure/`, `Mini_Memory/`, `Tunnels_Of_Doom/` and `Oldies_But_Goodies/` subfolders.
- [github.com/sonic2000gr/TI99](https://github.com/sonic2000gr/TI99) - one person's tapes
  from 1984-87, BASIC on side A and Extended BASIC on side B, BSD-2-Clause.
- [TOSEC TI-99/4A on archive.org](https://archive.org/details/Texas_Instruments_TI-99_4a_TOSEC_2012_04_23)
  and the [AtariAge TI-99/4A forum](https://forums.atariage.com/topic/247784-good-source-for-downloading-cassette-programs/).

Copy one into `/saves/ti99/tapes/` and load it with `OLD CS1`. Names longer than 24
characters are skipped rather than truncated, so rename anything long.

`make -C tools/tapetest run` exercises the encode/decode path, the 9901 timer and the CRU
decode on the host, without hardware. Point it at a dump to include that too:

```
TAPETEST_REAL=/path/to/dump.wav make -C tools/tapetest run
```

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

The LPC bitstream is read **least significant bit first within each byte** - the order
the TMS6100 shifts its serial data out, and the order cartridge speech data is stored in.
Fields are then assembled most significant bit first out of that stream, so each byte is
effectively read back to front. Getting this backwards does not fail loudly: the fields
land on the wrong bits, the stream drifts, and eventually a 4-bit energy field reads as
15 - a stop frame - and the phrase cuts off partway through. That was the cause of the
"no speech in Parsec" bug; all 22 of Parsec's phrases now decode to exactly their
declared byte count and end on a real stop frame.

Output level is set so a loud phrase peaks at about 10700, which is where one PSG channel
at full volume sits, so speech carries over the game without the mixer clipping when both
are busy.

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
| SELECT | Types **1** - picks the first option on a cartridge's title screen |
| START | Types **2** - picks the second |
| SELECT + START | Settings menu (cassette lives here) |
| SELECT + UP/DOWN | Screen mode |
| START + A | Toggle FPS display |
| SELECT + START + UP + A | Reboot into BOOTSEL mode |

Nearly every cartridge opens on the master title screen asking for a number, so SELECT and
START are mapped to **1** and **2**. That is enough to start most games without a keyboard
attached. Neither types anything while it is being held as part of one of the combinations
above, so opening the settings menu does not put a 1 into the running game.

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

## Memory

The RP2350 has 512 KB of SRAM and the framework's framebuffer already takes 150 KB of it,
so where each buffer lives is a deliberate choice rather than an accident.

**PSRAM where the board has it, SRAM otherwise.** The machine's two 64 KB address spaces -
CPU and GROM - are the largest thing it needs, so they move to PSRAM on boards that have
it and give back 128 KB of SRAM. Boards without PSRAM fall back to SRAM and run exactly as
before; nothing is excluded.

**SRAM always.** Video RAM (16 KB) stays put: the VDP walks it for every scanline it
renders, and 16 KB is not worth the latency. Cartridges up to 64 KB stay in SRAM too -
every fetch from `>6000` goes through them.

**PSRAM only, feature off without it.** The disk controller DSR (8 KB), the speech
vocabulary ROM (32 KB), Super Cart RAM (32 KB), SAMS memory, and cartridges over 64 KB.

The reason for deciding all of this up front rather than at the point of allocation: the
SDK's `malloc` **panics** instead of returning NULL, so "try SRAM, fall back to PSRAM"
cannot work - the fallback is unreachable and the board resets instead. Every allocation
is therefore sized against a known budget, and anything that might not fit is either
gated on PSRAM or routed to it.

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

- **`CALL SAY` / resident vocabulary is unverified.** The vocabulary ROM read path shares
  the bit-order fix described under [Speech](#speech), but only cartridge speech has been
  checked against real data so far.

## Not yet done

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
