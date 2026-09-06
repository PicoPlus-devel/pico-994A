# Changelog

## v0.1.0 - initial port

First working build of the TI-99/4A on RP2350.

### Emulation
- TI-99/4A core ported from [DS994a](https://github.com/wavemotion-dave/DS994a):
  TMS9900 CPU, TMS9901 CRU (keyboard, joysticks, timer), TMS9918A video,
  SAMS memory expansion, TI Disk Controller, `.rpk` cartridge loading.
- 32K Memory Expansion always fitted.
- SN76489/SN76496 sound: upstream's ARM32 assembly core replaced with the C core
  from pico-smsplus, since the original will not build for Cortex-M33.

### Front end
- Video, audio, menu, settings, SD card and USB input via `pico_shared`.
- 256x192 picture centred in the 320x240 active area, with the borders taking the
  backdrop colour from VDP register 7.
- Full USB keyboard support mapped onto the TI keyboard matrix, including FCTN,
  CTRL, SHIFT and a latching ALPHA LOCK. Characters a PC types with Shift but the TI
  keeps elsewhere (`"` `_` `{` `}` `|` `?` `~`) come out right.
- Two joystick ports from USB gamepads, GPIO NES/SNES pads and Wii controllers.
- TI BASIC reachable through a `.tib` marker file, created automatically on first run.
- Disk images kept in `/saves/ti99/disks/` can be put into DSK1, DSK2 or DSK3 from the
  settings menu while a game is running, as well as being mounted automatically from
  beside the cartridge. Blank 360 KB disks can be created there too. Tapes are chosen
  from the same kind of list.
- Builds for all 11 RP2350 hardware configurations plus the bootloader variant.

### Memory work needed to fit RP2350
The DS has 4 MB and spare video RAM; the RP2350 has 512 KB shared with a 150 KB
framebuffer. Four changes, in order of size:

- **`CompareZeroLookup16` (128 KB -> 0).** The table held one three-line expression
  per 16-bit index; it is now computed inline, which is also faster than the load it
  replaced.
- **`OpcodeLookup` (128 KB -> 4 KB).** Every mask in the decode chain is `0xf000`,
  `0x0f00`, `0x0c00`, `0x00e0` or `0x00c0`, so the instruction class never depends on
  bits 3..0 - one entry per 16 opcodes is exact - and the value was already cast to
  `u8` at every read site. Verified exhaustively over all 65536 opcodes against the
  original table before the change was made.
- **Disk images (720 KB -> 0).** Upstream buffers whole 360 KB `.DSK` images in RAM
  and writes dirty sectors back on unmount. Sector I/O now goes straight to the card
  through a handle held open while the disk is mounted, so a power cut cannot lose a
  saved program.
- **`XBuf` (96 KB -> 256 bytes).** Upstream keeps two full 256x192 frames and blits one
  at vblank. Scanlines are now converted into the framebuffer as the VDP produces them.
  Safe against tearing because the emulation loop paces to vsync *before* running a
  frame, so the writer starts at line 0 during vblank and stays ahead of scanout.

All per-game buffers are allocated at game start and freed on return to the menu, so
the `pico_shared` menu keeps the heap it needs on boards without PSRAM.

Resulting budget (Fruit Jam, HW_CONFIG 8, the heaviest static footprint):

| | bytes |
|---|---|
| Static (`.data`/`.bss`, incl. the 150 KB framebuffer) | 265,320 |
| Free for the heap | 258,968 |
| Emulated machine: MemCPU 64K + MemGROM 64K + VRAM 16K + disk DSR 8K | 155,648 |
| Cartridge (typical 8 KB / worst case in SRAM 64 KB) | 8,192 - 65,536 |
| **Spare during play** | **~46 KB - 95 KB** |

Cartridges over 64 KB go to PSRAM; boards without it will refuse those carts rather
than fail obscurely. SAMS memory is PSRAM-only and falls back to the plain 32 KB
expansion when no PSRAM is fitted.

### Bugs found and fixed while porting
- **Cartridge bank switching could read past the end of the heap block.** `WriteBank()`
  masks the bank with `BankMasks[]`, which rounds *up* to a power of two, so a five-bank
  cart can legally be asked for bank seven. Upstream never notices because it allocates
  a fixed 512K; with the buffer sized to the cart, `ti99_cart_alloc()` now rounds up to
  a power-of-two bank count so the whole masked range is always covered.
- **Super Cart wrote 8 KB past the end of the cart buffer.** Its 32 KB of banked RAM was
  addressed as `MemCART + (MAX_CART_SIZE - bank*0x2000)`, and `bank` starts at 0 - which
  indexes one bank beyond the buffer on the very first switch. Replaced with a dedicated
  32 KB allocation made only when that cart type is active.
- **Frameskip "off" still dropped one frame in 256** (mask `0xFF`, so `idx & mask == 0`
  once per wrap) - a visible hiccup every four seconds. Index 0 now means never skip.
- **Cart buffer was sized from the selected file.** Picking the `G` part of a C/D/G set
  sized the buffer from the GROM, truncating the ROM; it is now sized from the `C` part.

### Speech Synthesizer
Written for this port rather than ported: DS994a has no synthesiser, only a table that
fingerprints the first four bytes of each Speak External command and plays a matching
pre-recorded sample through the DS sound library, for twelve known titles.

`ti99/speech.c` is a real TMS5200 - the LPC-10 model from TI's patents (US 4,209,804
lattice filter, 4,331,836 chirp excitation, 4,335,277 frame and command logic) with the
chip's published coefficient tables. Ten-stage lattice at 8 kHz, parameters interpolated
across eight periods per 25 ms frame, resampled into the mixer with a 16.16 phase
accumulator. Both the TMS5200 (what the TI-99/4A shipped with, the default) and TMS5220
table sets are included.

Cost: ~340 bytes of tables in flash, ~400 bytes of state, and well under 1% of the CPU -
the same algorithm runs on a 16 MHz AVR in the Talkie library. The 32 KB `spchrom.bin`
resident vocabulary is only allocated when the file is on the card; cartridge speech uses
Speak External and needs no vocabulary ROM at all.

Two things the test harness caught that would have been silent failures on hardware:
- **Data bytes were being decoded as commands.** While Speak External is active the chip
  does no command decoding at all - every write is a FIFO byte. LPC data routinely
  contains bytes that look exactly like `Reset` or `Load Address`.
- **Speak External does not start the chip talking.** It arms the FIFO; synthesis begins
  on the falling edge of buffer-low, once enough bytes have arrived. Starting immediately
  meant the first frame was parsed from an empty FIFO and read as a stop frame.

### Fixed after first hardware test
- **No speech in Parsec: the LPC bitstream was being read the wrong way round.** The
  frame decoder pulled bits most significant first; TI's LPC data runs *least*
  significant bit first within each byte, which is the order the TMS6100 shifts its
  serial data out and the order cartridge data is stored in (it is why the Talkie library
  bit-reverses every byte). This failed quietly rather than loudly - the fields simply
  landed on the wrong bits, the stream drifted, and sooner or later a 4-bit energy field
  read as 15, a stop frame, cutting the phrase off partway through. Half of Parsec's
  phrases died within the first few frames.

  The synthesiser's own harness could not have caught it: it fed synthetic frames that
  the harness itself encoded, in the same wrong order, so they round-tripped perfectly.
  What found it was replaying Parsec's real driver - disassembled from `PARSECC.bin`
  at `>7F4E`, where it writes `>60` and then streams GROM bytes straight to `>9400`,
  polling Buffer Low at `>9000` and topping up 8 bytes at a time - against the real LPC
  data in `PARSECG.bin`. All 22 phrases now consume exactly their declared byte count and
  end on a real stop frame, with pitch and voiced/unvoiced structure to match.
- **Speech was ~12 dB too quiet.** With the phrases decoding, they were still barely
  audible next to the game. The output stage assumed the lattice filled a +-16384 range
  and halved it for headroom; in fact the lattice runs at the chip's 14-bit scale
  (+-8192) and real speech peaks near 5400, so a loud phrase came out at 2682 - about a
  quarter of a single PSG voice. It now doubles instead of halving, putting a loud phrase
  at ~10700 against the 10922 a PSG channel reaches at full volume, measured over all 22
  Parsec phrases with no clipped samples. Pure gain change; the waveform is untouched.
- **The PSG's DC offset was clipping the mix.** The SN76496 core's output is unipolar -
  `vol[]` counts how long each square sits in its 1 position, so the channel sum runs
  0..MAX_OUTPUT and carries a DC offset of about half its own amplitude. Alone that is
  inaudible, but it costs the entire negative half of the range, and with speech mixed on
  top the sum only ever clips against the positive rail. A capture of Parsec's opening had
  1781 clipped samples, **every one at +32767 and none at -32768**, and all 1781 were
  within reach of the ceiling purely because of the offset they rode on. `ti99_psg_mix()`
  now tracks the offset with a one-pole low pass at ~7 Hz - well below anything the PSG
  produces - and subtracts it. Speech itself needed no change: only 21 samples in 225600
  across all 22 Parsec phrases exceed the headroom the 4x HDMI output gain leaves.

  The offset estimate is carried in Q14, not at sample precision, and that matters. The
  obvious direct form, `y = x - x1 + (1023/1024)*y1`, feeds back through a *floor* divide
  on a signed value, and for any negative state with `|y| < 1024`,
  `floor(y * 1023/1024) == y` - so negative offsets are a fixed point and never decay
  while positive ones bleed away. Every sound that ended left the output parked on a DC
  step, which is what crackling in an otherwise silent passage sounds like. With 14
  fractional bits the residual stays below a fraction of one sample unit and digital
  silence in gives bit-exact silence out.
- **Hardfault booting TI BASIC.** `TMS9900_Reset()` cleared a fixed 512 KB of `MemCART`,
  which is what the DS allocates up front. Here the cart buffer is sized to the
  cartridge and is not allocated until *after* the reset runs, so this was a 512 KB
  write through a null pointer. Now clears exactly what exists, which on the way into a
  new game is nothing.
- **`rpk_load_paged7` scribbled 32 KB past the cart buffer.** It used `MemCART+0x10000`
  as scratch - free space inside the DS's fixed buffer, well past the end of ours. Uses
  a real scratch allocation and checks the image will fit before building it.
- **Disk sector transfers could write outside video RAM.** The DSR's VDP buffer address
  was used unmasked, but the VDP address bus is 14 bits; a transfer near the top of the
  range ran off the end of the 16 KB heap block. Masked and clamped to the window.

`pcode.c` reads `MemCART[0x10000 + ...]` on the same assumption, but the p-code card is
never enabled in this port so that path is unreachable.

### Allocation strategy
The SDK's `malloc` panics instead of returning NULL, so a shortfall shows up as a board
reset and "try SRAM, fall back to PSRAM" cannot work - the fallback is unreachable. Every
allocation decision is now made from the size up front:

- **Preferred PSRAM** (`ti99_mem_alloc` -> `Frens::f_malloc`) for the buffers the machine
  cannot run without: the 64 KB CPU and 64 KB GROM address spaces. On a board with PSRAM
  that frees 128 KB of SRAM; without PSRAM it falls back to SRAM and behaves as before, so
  no hardware configuration is dropped.
- **SRAM** keeps video RAM (16 KB, walked every scanline) and cartridges up to 64 KB
  (every fetch from `>6000` goes through them).
- **Strict PSRAM** (`ti99_psram_alloc`, returns NULL rather than panicking when there is
  none) for everything optional, which is then simply switched off: disk controller DSR,
  speech vocabulary ROM, Super Cart RAM, SAMS memory, cartridges over 64 KB.
- `ti99_cart_alloc` no longer tries SRAM and falls back; it picks by size.
- Removed a `free()` of `SharedMemBuffer`/`SharedMemBufferBig`, upstream's DS scratch
  buffers, which this port never allocates.

### Controls
SELECT and START now type **1** and **2** on the TI keyboard during emulation. Nearly
every cartridge opens on a title screen asking for a number, so without this a gamepad on
its own could not start a game. Neither fires while the button is being held as part of an
emulator shortcut - SELECT pairs with START and the d-pad, START with SELECT, A and
left/right - so opening the settings menu no longer types a 1 into the running game.

### Menu
- Allowed-extension lists in `pico_shared`'s RomLister are **space** separated, not
  comma separated (`RomLister::IsextensionAllowed` splits on `' '`). The first build
  passed `".rpk,.bin,.tib"`, which was treated as one 14-character extension and matched
  nothing, so the ROM browser came up empty.
- A TI-99/4A cartridge is often several files - `GAMEC.bin`, `GAMED.bin`, `GAMEG.bin`,
  `GAME0.bin` - and selecting any one of them loads the whole set, so listing every part
  turned one cartridge into three or four menu entries. RomLister now hides the D/G/0
  parts when their C/8/9 primary is in the same folder. GROM-only cartridges have no
  primary and stay visible. Gated on the emulator type, so it is a no-op for every other
  emulator (which also use `.bin`).

### Cassette (CS1/CS2)
Written for this port; DS994a has no cassette support at all. There is no sector call to
trap - the console DSR bit-bangs the TMS9901 (CRU 22/23 motor, 24 audio gate, 25 out,
27 in) and times the bit cells against the 9901 interval timer - so what is emulated is
the signal in time rather than a file interface.

Everything hangs off one decision: **the tape position is a function of `tms9900.cycles`,
not of the frame loop.** The cycle counter advances at 3 MHz whatever the emulator is
doing and the 9901 timer is derived from the same counter, so the DSR's stopwatch and the
tape it is measuring cannot drift apart. It also costs nothing with no tape loaded - no
tick, no buffer, no allocation - and gives exact resolution when there is one.

Two formats: `.wav` for interoperability (any PCM rate, 8/16-bit, mono/stereo, so real
cassette dumps load unchanged) and `.cas` for size (one bit per cell, ~170 bytes/second).
Tapes live in `/saves/ti99/tapes/`; the deck is a settings-menu entry using the same hook
mechanism the NES build uses for FDS disk swapping, and opens a list to choose from - the
same list the disk drives use.

`SAVE CS1` supplies no filename - the device name is all the TI gives, a tape has no
directory, and nothing in the data carries a name - so recording asks for a label, typed
on the USB keyboard. That needed a `showTextEntry` widget in `pico_shared` which reads
keyboard state *without* polling pad state, because `hid_app.cpp` maps the same HID report
onto a gamepad slot: `A`->SELECT and `S`->START, so a dialog that polled pads would treat
typing "SAM" as menu navigation and walk out of itself.

**The deck follows the console rather than waiting to be set up.** The first version put
arming behind a settings-menu entry, which meant `SAVE CS1` on an unarmed deck wrote into
nothing: no prompt, no audio, and then `ERROR - NO DATA FOUND` and `I/O ERROR 66` when the
verify pass found an empty tape. Nothing had gone wrong mechanically - there was simply no
tape in the machine and nothing said so. The DSR does not announce its intent, but its
first CRU access does: a write to the data line is a save, a read is a load. Either one
against an empty deck now raises a request the frame loop turns into a prompt, which lands
at exactly the point the console is telling the user to press RECORD or PLAY. One ask per
motor-on pass, so declining it does not nag.

For the same reason the deck turns itself round for `CHECK TAPE`: a read while recording
finalises the file and reopens it rewound, because that is what "REWIND CASSETTE TAPE /
PRESS CASSETTE PLAY" means and the alternative was a trip to the menu mid-prompt.

And it winds back at the start of every load, for the same reason. After a save and its
verify the deck is still loaded and parked at the end of the tape, so `OLD CS1` in the
same session found a deck that was not empty - no prompt - and read off the end into
silence: `NO DATA FOUND` again, with nothing to say a tape was even involved. A read
arriving after 250ms of quiet is a new load; within one, the DSR polls every half cell.

That has to include the *first* read after a tape is loaded, which the first version
missed. Because the console never drops the motor relay, the transport keeps running from
the moment a tape is picked in the menu until the user has typed `OLD CS1` and answered
two prompts - by which point the tape is sitting seconds past the data. Mounting resets
the read history, so keying only off the gap since the last read left exactly that case
unable to wind back.

The underlying oddity is that the emulated transport follows the motor relay while a real
one follows the buttons on the deck, and the console asks the *user* to press stop. That
is also where the silence on the end of a saved tape comes from. The rewind rules make it
behave correctly; the tails are cosmetic.

Recording is also audible without waiting on the audio gate, and the file header is
patched at every motor stop rather than only on close - answer N to CHECK TAPE and the deck
stays armed indefinitely, and a WAV with a zero-length data chunk is a file nothing can
open.

#### Bugs this uncovered in the existing core
- **The 9901 timer was not usable as a stopwatch.** It was decremented by a flat 3 ticks
  once per scanline, which averages out near the real 46.9 kHz but quantises every value
  the CPU reads to a 63.7 us boundary in steps of 3. Against a bit cell of ~700 us that is
  enough jitter to corrupt the decode. The counter is now derived from the cycle count on
  demand. Two details matter: the decrementer keeps running while the CPU is in clock mode
  (clock mode latches a snapshot for reads, it does not stop the clock), and the sub-tick
  remainder has to be carried - the DSR enters and leaves clock mode on every measurement,
  so discarding it made the clock lose up to a tick per read. Resolution is now a single
  64-cycle tick.
- **`CRU_AliasTable` was wrong, and bit 27 landed on the keyboard.** Upstream mapped read
  bits 23-31 down by 8, so **a read of CRU 27 - the cassette input - returned pin 19,
  `PIN_COL2`, the keyboard column select**. Only entry 23 was correct. In fact the
  TI-99/4A needs no aliasing at all: reads of bits 16-31 return the I/O pins directly,
  which is what the DSR depends on when it reads back the motor and gate bits it just set.
  Table removed.
- **The alias overwrote the CRU loop variable.** It assigned back into `cruAddress`, which
  was then incremented at the bottom of the loop, so a multi-bit `STCR` reaching into the
  aliased region walked the wrong addresses from the second bit on. Latent because only
  the identity-mapped keyboard bits (3-10) were ever read.
- **A CRU write from clock mode threw the data bit away.** On the 9901 a write to a pin
  above 15 does two things: it drops the chip out of clock mode, *and* it drives the pin.
  Upstream only did the first. Nothing noticed because clock mode is used by exactly one
  thing - the cassette DSR, which times its bit cells against the timer and therefore
  writes the data line from inside clock mode. **Every data bit of a SAVE was discarded**,
  so the tape came out holding a single unbroken level and the verify pass reported
  `ERROR - NO DATA FOUND`. The pin write is now factored into `TMS9901_WritePin()` and
  called from both paths.

  Worth noting how this got past the first round of tests: the obvious assertion is that
  recording produced a file of about the right size, and it did. A frozen data line still
  fills the file with PCM at whatever level it stuck at, which is exactly how a blank tape
  passes for a recording. The test now counts transitions in the written WAV.
- **And the same thing on the read side.** Clock mode only redefines CRU bits 0-15; bits
  16-31 stay the I/O pins whatever mode the chip is in. Upstream ran the clock decode over
  all 32, so a poll of bit 27 - which the DSR does from inside clock mode, because that is
  where it is timing the cell from - came back as a bit of the timer register. With the
  write fixed the tape recorded correctly and *still* verified as `NO DATA FOUND`, because
  the read never reached the tape. It also meant the deck never noticed the console had
  started reading, so a save left ~19 s of tape rolling behind it.

  The first attempt at a test for this passed with the fix reverted: it parked the data
  line at a constant level, and an AC-coupled detector reads a constant as nothing either
  way. It now plays a tape with real transitions and compares the same read taken in I/O
  mode and clock mode at the same instant - 505 of 2000 differ without the fix.
- Turning the deck round for `CHECK TAPE` waits for the data line to go quiet, because
  once clock-mode reads reached the tape a glance at the input mid-save would have flipped
  to playback and truncated the recording. The first version of that guard keyed on the
  motor being cycled, which does not happen: **the console leaves the relay closed right
  through "PRESS CASSETTE STOP", "REWIND" and "PRESS PLAY"** - it is telling the user to
  stop the recorder by hand. A tape recorded on hardware shows it plainly, 5.6s of data
  followed by 69s of transport still rolling. So the guard is a quiet period on the data
  line instead: a save writes at least one transition per 2304-cycle bit cell, so 50ms of
  silence cannot be one.

  The test had cycled the motor, which hid this completely. It now models the tape running
  on through the prompts, and reverting to the motor-based guard makes it report `verify
  pass recovered only 0 bits of 120 - the tape read back empty`.

#### Bugs the host tests found in the new code
`tools/tapetest` builds the real `cassette.c` and `tms9901.c` against stubs and drives
them through `TMS9901_WriteCRU`/`ReadCRU` exactly as the CPU would. It caught three
things that would have been miserable to chase on hardware:

- **The level detector and the audio monitor were two independent readers** of the same
  file window, at different positions. Each would have dragged the file pointer back past
  the other, putting SD reads inside CRU reads - the one thing the per-frame tick exists
  to prevent. The monitor now reads a ring of what the detector has already consumed, so
  only one cursor ever walks the tape.
- **Seeding the detector baseline from one sample made it worse, not better.** The tape is
  a square wave, so any single sample sits at a peak rather than the centre; priming from
  it put the baseline a full amplitude out and a zero-DC tape decoded to nothing at all.
  It primes from the mean of the first 64 samples instead.
- **The `.cas` bit-rate estimator had a stuck fixed point.** `(15*7 + 16)/8 == 15` in
  integer arithmetic, so the one-pole filter sat on its initial guess for ever and looked
  like it was calibrating - it reported 1470 baud for a 1379 baud tape. Carried in Q8 it
  converges to 1380. The same floor-divide trap as the PSG DC blocker above, which is why
  that one is Q14.

Because the estimator writes the measured cell period into the `.cas` header, playback
uses the rate the console actually recorded at rather than a hardcoded assumption. The
`.wav` path never had that dependency - it records the waveform itself.

The fallback rate is no longer a guess either. Measured off a tape the console wrote, a
cell is 768 us - 2304 CPU cycles, or exactly 36 ticks of the timer the DSR counts - which
is **1302 baud**. The commonly quoted ~1379 is 6% out.

Reading a dump of a physical cassette found two things nothing else had. **Real dumps are
commonly 32-bit IEEE float at 48kHz** - that is what a PC recording program writes without
being asked - and the reader only took 8- and 16-bit integer PCM, so it refused them
outright. It now takes 8/16/24/32-bit integer and 32-bit float, and resolves
`WAVE_FORMAT_EXTENSIBLE` through its SubFormat GUID. And `tapeinfo.py` split the intervals
into short and long by looking for the widest gap between observed lengths, which works
only on synthetic data: a real tape's speed wanders, so each group is a spread rather than
a single value. It clusters properly now.

With those fixed, a dump of a real cassette decodes with a 767-byte `>00` leader against
the real 768, and the emulator's own detector reads it to 6139 leader bits of a possible
6144 followed by the `>FF` marker. That is the first check of the threshold logic against
a tape that went through an actual recorder, with the DC offset and level drift it exists
to cope with; `TAPETEST_REAL=/path/to/dump.wav make -C tools/tapetest run` runs it.

`tools/tapeinfo.py` reports what is on a tape - transition count, interval clustering,
the implied bit rate, and the decoded leader and `>FF` marker. It is how a recording gets
checked against a real cassette dump rather than against our own encoder, and it is what
tells a blank tape (`0 transitions`) apart from a bad decode.

### Not implemented
Save states, p-code card.

### pico_shared
Requires the `ti99` branch: adds the `TI99` emulator type, its settings-visibility
array, `.rpk`/`.tib` extension handling (with `.bin` disambiguated against Mega Drive
by the build's own emulator type), and a `/Metadata/TI99` artwork probe.

Cassette support adds to it, all additively - no existing signature changes, and sibling
emulators pick up one hidden menu slot and one unused flag:
- `MenuCassetteHooks` and `menuSetCassetteHooks()`, modelled on the existing
  `MenuFdsHooks`, plus the `MOPT_CASSETTE` option (appended, per the append-only rule in
  `menu_settings.h` - sibling emulators size their visibility arrays positionally).
- `showTextEntry()`, a modal single-line text prompt.
- `KeyboardState::connected`, mirroring `MouseState::connected`, so a caller can tell
  whether there is a keyboard to type on. There was no way to know before.
