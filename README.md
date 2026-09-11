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
- **Disk controller** - DSK1, DSK2 and DSK3 against `.DSK` sector images, read and write,
  mounted beside the cartridge or chosen from the menu. Needs a board with PSRAM (see
  [Memory](#memory)).
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
| `spchrom.bin`  | 32 KB | no      | Speech Synthesizer resident vocabulary, PSRAM boards only - see [Speech](#speech) |

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
On a board without PSRAM the console's own 144 KB comes out of that same heap, so the
practical ceiling there is 32 KB of cartridge ROM - a larger cart is refused with a
message rather than being loaded into memory that is not there. Cartridge GROM is not
part of this: it lives in the 64 KB GROM space and is unaffected by the cart size.

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
360 KB `.DSK` sector images (v9t9 format). There are two ways to get a disk into a drive.

**Automatically, from beside the cartridge.** For `GAME.bin` the emulator looks for
`GAME1.dsk`, `GAME2.dsk` and `GAME3.dsk` in the same folder and mounts whichever of them
it finds as DSK1, DSK2 and DSK3.

**By hand, from the settings menu.** Put loose images in `/saves/ti99/disks/` and open the
settings menu while a machine is running (SELECT + START). The `Disk` entry addresses one
drive at a time:

| Key | What it does |
|-----|--------------|
| LEFT / RIGHT | step between `Disk DSK1`, `Disk DSK2` and `Disk DSK3` |
| `A` | open the list of images for the drive shown |
| UP / DOWN | move through the list |
| `A` | mount the highlighted image, `<no disk>` to eject, or `Create blank disk` |
| `B` | leave the list without changing anything |

A drive keeps its disk until you change it or load another cartridge. The entry is offered
only while a machine is running, since loading a cartridge empties every drive.

### Creating a blank disk

`Create blank disk`, the last line of that list, formats a new image and puts it straight
into the drive you opened the list from. It asks for a name first, which is written both
to the file and to the disk's own volume header, so a catalogue reads back the same name
the menu shows. Without a USB keyboard attached there is nothing to type with, so it
offers the first unused name instead - `DISK01`, `DISK02` and so on - and asks you to
confirm that before formatting anything.

The image is 360 KB: 1440 sectors, 40 tracks, 18 sectors per track, double sided and
double density. That is the largest geometry the TI Disk Controller handles, and writing
it out takes a second or two on the card.

Nothing else is needed to start using it - a freshly formatted disk is ready for
`SAVE DSK1.MYPROG`. There is no need to run Disk Manager over it first, since the volume
header and the empty file index are written as part of formatting.

TI BASIC started from the `.tib` marker boots with no cartridge, so there is no name for
anything to be auto-mounted against - mount a disk from the settings menu instead.

Sector writes go straight to the card rather than being buffered and written back on
exit, so pulling the power will not lose a program you just saved.

### Disk commands

A filename is `DSKn.NAME`, where `n` is 1, 2 or 3 and `NAME` is at most ten characters.
These work in both TI BASIC and TI Extended BASIC:

| Command | What it does |
|---------|--------------|
| `OLD DSK1.MYPROG` | load a program into memory |
| `SAVE DSK1.MYPROG` | save the program in memory |
| `LIST "DSK1.MYPROG"` | write a listing out as a DIS/VAR 80 text file |
| `DELETE "DSK1.MYPROG"` | delete a file |
| `OPEN #1:"DSK1.DATA",INPUT,INTERNAL` | open a data file; `PRINT #1`, `INPUT #1`, `RESTORE #1`, `EOF(1)` and `CLOSE #1` follow as usual |
| `CALL FILES(n)` | set how many files may be open at once, 1 to 9. Clears the program in memory, so use it first |

TI Extended BASIC adds:

| Command | What it does |
|---------|--------------|
| `RUN "DSK1.MYPROG"` | load and run in one step (TI BASIC's `RUN` takes only a line number) |
| `SAVE DSK1.MYPROG,PROTECTED` | save so the program cannot be listed, edited or re-saved |
| `SAVE DSK1.MYPROG,MERGE` | save in MERGE format (DIS/VAR 163) instead of a program file |
| `MERGE DSK1.MYPROG` | merge such a file into the program already in memory |
| `CALL INIT` / `CALL LOAD("DSK1.OBJ")` / `CALL LINK("START")` | load and call assembly language |

Extended BASIC also runs `DSK1.LOAD` by itself at startup if that program file exists,
which is how most program disks put up their own menu. That happens when the cartridge
starts, so a disk mounted from the settings menu afterwards has already missed it - reset
from the settings menu, or just type `RUN "DSK1.LOAD"`.

### Listing a disk

Neither BASIC has a `CATALOG` command - cataloguing a disk was the job of the Disk Manager
cartridge. What the disk controller does provide is the directory itself, as a read-only
file named `DSKn.` with no filename after the dot. Record 0 carries the disk name, its
size and its free space; each record after that is one file. This program prints a
catalogue and works in both TI BASIC and Extended BASIC:

```
100 DIM T$(5)
110 T$(1)="DIS/FIX"
120 T$(2)="DIS/VAR"
130 T$(3)="INT/FIX"
140 T$(4)="INT/VAR"
150 T$(5)="PROGRAM"
160 OPEN #1:"DSK1.",INPUT ,RELATIVE,INTERNAL
170 INPUT #1:N$,X,TOT,FRE
180 PRINT "DISK ";N$
190 PRINT "USED";TOT-FRE;"FREE";FRE
200 PRINT
210 FOR I=1 TO 127
220 INPUT #1:N$,TY,SZ,RL
230 IF LEN(N$)=0 THEN 290
240 P$=" "
250 IF TY>0 THEN 270
260 P$="P"
270 PRINT N$;TAB(12);T$(ABS(TY));SZ;P$
280 NEXT I
290 CLOSE #1
```

`TY` is the file type - 1 to 5 as named in `T$` above - and is negative when the file is
protected, which is what the trailing `P` marks. `SZ` is the size in sectors of 256 bytes.

This works here because the emulator does not intercept disk commands: it traps only the
controller's read-a-sector and write-a-sector routine, so the real DSR in `994aDISK.bin`
does the directory work exactly as it would on a real disk controller card.

## Cassette

The cassette port works, in both directions:

```
SAVE CS1
OLD CS1
```

Tapes live in `/saves/ti99/tapes/` and the deck is in the settings menu (SELECT + START).
The `Cassette` entry shows what is in the deck; press `A` to open the list:

| Entry | What it does |
|-------|--------------|
| `<no tape>` | eject, leaving the deck empty |
| a tape name | put that tape in the deck, wound to the start |
| `Record (WAV)` | record a new tape as a `.wav` |
| `Record (CAS)` | record a new tape as a `.cas` |
| `Rewind` | wind the loaded tape back (offered while one is playing) |

UP/DOWN move through the list, `A` takes the highlighted line and `B` leaves without
changing anything - nothing is opened or created until you press `A`. Use it to choose
`.cas` instead of `.wav`, to rewind for a second read, or to eject.

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

### Making a tape from a program file

`tools/mktape.py` writes a tape from a V9T9 FIAD, a TIFILES file, or a raw program image -
whatever the console would have written had you typed `SAVE CS1`:

```
$ tools/mktape.py A13
A13.wav
  from A13 (FIAD), 640 byte image, 10 records
  2251 bytes on tape, 13.8s at 44100 Hz
```

Useful for getting a program that only exists as a disk file onto tape, and for Extended
BASIC, where no cassette dumps seem to be published at all.

The format was read off tapes the hardware wrote and checked against the matching FIAD
files, not guessed: 768 bytes of `>00`, `>FF`, the record count twice, then each 64-byte
record written out twice with a checksum. Tapes it produces come back byte-identical to
the originals, which is the check that it is right.

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
is only loaded when present, so it costs nothing if you leave it out. Like the disk
controller DSR it is held in PSRAM, so it is not loaded at all on a board without one
(see [Memory](#memory)); the module itself still works there.

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

A USB keyboard maps onto the TI keyboard directly. Letters, digits, Enter, Space and
`.` `,` `/` `;` `=` are where you expect, and the shifted number row matches the TI's
(`!@#$%^&*()`), as do `:` `<` `>` and `+`.

| PC key | TI equivalent |
|--------|---------------|
| Left Alt / Right Alt | **FCTN** |
| Ctrl | **CTRL** |
| Shift | **SHIFT** |
| Caps Lock | **ALPHA LOCK** (latching, as on the real machine) |

### Editing and control keys

| PC key | TI keys | Meaning |
|--------|---------|---------|
| Arrow keys | FCTN + E / S / D / X | up / left / right / down |
| Backspace | FCTN + S | left - the TI has no destructive backspace |
| Delete | FCTN + 1 | DEL, delete the character under the cursor |
| Insert | FCTN + 2 | INS, start inserting |
| Escape | FCTN + 9 | BACK, leave the current activity |
| F1 - F9 | FCTN + 1 - 9 | DEL, INS, ERASE, CLEAR, BEGIN, PROC'D, AID, REDO, BACK |
| F10 | FCTN + = | QUIT |

QUIT is not a menu key. It resets the console to the title screen and discards the program
in memory, exactly as on real hardware.

### Punctuation the TI keeps elsewhere

Both spellings work: type the character as you would on a PC, or as a TI user would with
FCTN.

| Character | PC | TI |
|-----------|----|----|
| `-` | `-` | SHIFT + `/` |
| `'` | `'` | FCTN + O |
| `"` | Shift + `'` | FCTN + P |
| `_` | Shift + `-` | FCTN + U |
| `?` | Shift + `/` | FCTN + I |
| `[` `]` | `[` `]` | FCTN + R / T |
| `{` `}` | Shift + `[` `]` | FCTN + F / G |
| `\` | `\` | FCTN + Z |
| `\|` | Shift + `\` | FCTN + A |
| `` ` `` | `` ` `` | FCTN + C |
| `~` | Shift + `` ` `` | FCTN + W |

The `-` and `?` rows are worth reading together. On a TI, SHIFT + `/` is the minus sign,
which is why the question mark had to go somewhere else entirely. Typing `?` the PC way
gives you `?` as expected, but the TI spelling of it is FCTN + I, not SHIFT + `/`.

FCTN on the remaining keys - B, H, J, K, L, M, N, Q, V, Y, 0, `;`, `.` and `,` - produces
no character.

These assignments are not guesswork: the console GROM carries three 48-byte key
translation tables, one each for the plain, SHIFT and FCTN layers, and this table is read
straight out of them. (The TI Extended BASIC manual in `assets/` is not a guide to them -
it documents the earlier TI-99/4, which had no FCTN key and put the cursor keys on SHIFT.)

### Pasting text over the serial port

Text sent to the board's serial console can be typed into the machine, which is the
practical way to get a BASIC listing written on a PC into TI BASIC or Extended BASIC
without typing it twice. Turn **Serial keyboard** on in the settings menu; it is off by
default and the setting is remembered.

Connect a [Raspberry Pi Debug Probe](https://www.raspberrypi.com/documentation/microcontrollers/debug-probe.html) or a USB-to-serial adapter to the board's UART pins. Normally this is GPIO 0 (TX) and GPIO1 (RX) and a GND pin. 
On the Adafruit Fruit Jam that is GPIO 44 (TX), GPIO 45 (RX) and GND on the 2x16 header - the same pins the emulator
already prints its startup banner on, so a working banner confirms the wiring.

Any terminal program will do, as long as it is set to **115200 8N1 with software
(XON/XOFF) flow control** and hardware (RTS/CTS) flow control turned off. With minicom:

```sh
minicom -b 115200 -D /dev/ttyACM0
```

then press `Ctrl-A` `O`, choose *Serial port setup*, and use `F` to set **Hardware Flow
Control** to `No` and `G` to set **Software Flow Control** to `Yes`. Turning hardware flow
control off matters: it is on by default in minicom, and with the usual three-wire
TX/RX/GND hookup it leaves CTS unasserted, so nothing you type is sent at all.

picocom takes the same settings on the command line:

```sh
picocom -b 115200 --flow x /dev/ttyACM0
```

Or with bash:

```sh
stty -F /dev/ttyACM0 115200 raw -echo ixon -crtscts
cat file.bas > /dev/ttyACM0
```

Both of those flags are easy to lose and neither fails loudly. `raw` implies `-ixon`, so
`ixon` has to come *after* it or there is no flow control at all. And `raw` leaves `ECHO`
untouched: with echo on, the host sends whatever the emulator prints straight back to the
board, which then types it into TI BASIC as though it had been keyed in.

`tools/copybas.sh` sets all of this for you, and is the easier way to do it.


The device name depends on the adapter: a Raspberry Pi Debug Probe and other CDC-ACM
adapters appear as `/dev/ttyACM0`, while FTDI, CP210x and CH340 adapters appear as
`/dev/ttyUSB0`. On Windows, PuTTY and Tera Term both offer XON/XOFF in their serial
settings.

Flow control is not optional for anything longer than a few lines. The console accepts
around twenty characters a second, while the serial port delivers eleven thousand, so the
emulator holds the sender off with XOFF while the machine catches up and releases it with
XON. The buffer is 16 KB, and the sender is only held off once three quarters of that is
in use, so an ordinary listing is taken in one go and never stopped at all - which also
means it arrives intact on a terminal that ignores XON/XOFF entirely.

That threshold is deliberately high. Stopping a sender that is about to finish can strand
the last few bytes of the file in its own transmit queue, where they are lost if the
writing process closes the port while still held off.

If the buffer does overflow, the emulator says so in the terminal rather than letting the
listing quietly corrupt:

```
Serial keyboard: input overflow - enable XON/XOFF flow control
```

At the end of a paste the emulator reports what it saw:

```
Serial keyboard: 2260 characters received, 2260 typed
```

If that count is short of the size of the file you sent, the missing characters never
reached the board and the fault is in the serial link or the sending program, not in the
emulator's typing.

If your terminal cannot do XON/XOFF at all, configure a per-character sending delay of
about 50 ms instead.

Typing paces itself against the machine rather than running to a fixed clock: each
keypress is held until the console has actually scanned the keyboard for it. That matters
because TI BASIC stops scanning altogether while it tokenises a line and scrolls the
screen, and anything typed into that window would simply be lost.

Incoming characters are collected by the UART interrupt rather than polled from the
emulation loop. The loop spends much of each frame waiting for vsync, and the UART's
32-byte hardware FIFO overruns in under 3 ms at 115200 baud, so a polled reader drops
whole runs of characters no matter how large the buffer behind it is.

`tools/copybas.sh` sends a listing for you, with the serial port set up correctly:

```sh
tools/copybas.sh examples/pastetest.bas /dev/ttyACM0
```

It clears the editor first (a few harmless statements, then `NEW`) so the listing arrives
in an empty machine, sends the file, and commits the last line. It also prints how long
the typing is expected to take, which is minutes rather than seconds.

Add `-w` and it waits for the board and checks the result:

```
$ tools/copybas.sh -w examples/pastetest.bas
copybas.sh: listing  examples/pastetest.bas - 2030 bytes, 53 lines
copybas.sh: sending  2115 bytes in all, counting the framing statements
copybas.sh: expect   about 3m15s of typing
...
copybas.sh: board | Serial keyboard: 2115 characters received, 2115 typed
copybas.sh: all 2115 characters reached the board
```

The emulator reports its totals on the serial port whenever it finishes typing a paste,
whether or not this script is listening. A count short of what was sent means those
characters never reached the board, which is a fault in the serial link or the sending
program rather than in the emulator's typing.

`examples/pastetest.bas` is a short TI BASIC program for checking all of this. Paste it
in and type `RUN`: it prints every character the TI keeps on its FCTN and SHIFT layers, so
anything mistyped is visible on screen, and it ends with `PASTE COMPLETE` so an incomplete
paste is obvious. It is about 2 KB, which takes roughly three minutes to type in.

Pasting is deliberately limited to text. All 95 printable ASCII characters are covered,
including the ones the TI keeps on the FCTN layer (`"` `?` `[` `]` `\` `|` `~` `_` `{` `}`
and `` ` ``), and each is sent with whatever SHIFT or FCTN the console expects. Carriage
return, line feed and CRLF all produce a single ENTER, a tab becomes a space, and
backspace maps to FCTN + S. Anything else - arrow keys, function keys, BREAK - is ignored;
use the USB keyboard for those.

Case is taken from the text itself rather than from ALPHA LOCK, so a listing pastes in as
written whichever way the ALPHA LOCK key happens to be latched.

To abandon a paste, send Ctrl-C, or simply press a key on the USB keyboard: a real
keypress always takes the machine back. Switching **Serial keyboard** off does the same.

Abandoning a paste discards the text still queued on the board, and then ignores whatever
the sender has left in its own transmit queue until the line has been quiet for half a
second. Without that second step the remains of the abandoned listing would arrive the
moment flow control was released, and type themselves into the next paste.

The setting only appears on boards that can actually receive. Murmulator M1 and M2 and the
deprecated HW\_CONFIG 11 have no serial console at all. The Pimoroni Pico DV Demo Base
(HW\_CONFIG 1) has one, but gives GPIO 1 - which is UART0 RX - to the second NES
controller's clock, so it can transmit and never receive: `printf` works there and input
cannot. The build works this out from the board's pin map, so the option is absent rather
than present and silently inert.

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
