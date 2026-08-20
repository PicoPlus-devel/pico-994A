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
  CTRL, SHIFT and a latching ALPHA LOCK.
- Two joystick ports from USB gamepads, GPIO NES/SNES pads and Wii controllers.
- TI BASIC reachable through a `.tib` marker file, created automatically on first run.
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

### Not implemented
Cassette (CS1/CS2), save states, p-code card.

### pico_shared
Requires the `ti99` branch: adds the `TI99` emulator type, its settings-visibility
array, `.rpk`/`.tib` extension handling (with `.bin` disambiguated against Mega Drive
by the build's own emulator type), and a `/Metadata/TI99` artwork probe.
