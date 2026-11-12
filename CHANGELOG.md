# CHANGELOG

**pico-994A** is a Texas Instruments TI-99/4A emulator for RP2350 boards (Raspberry Pi Pico 2
and variants), based on the [DS994a](https://github.com/wavemotion-dave/DS994a) emulation core
by wavemotion-dave. Cartridges, disks and tapes are loaded from an SD card, video and audio go
out over HDMI or DVI, and a USB keyboard works as the TI keyboard.

[Binaries for every board configuration are at the end of this page](#downloads___).

# Getting started

1. Format an SD card as FAT32 (recommended) or exFAT and copy your cartridges (`.rpk` files,
   or `.bin` sets) to `/roms/TI99`. Subdirectories are supported.
2. Copy the console system files `994aROM.bin` and `994aGROM.bin` to `/bios/` on the same card.
   They are not distributed with the emulator; without them nothing runs. `994aDISK.bin`
   (disk controller) and `spchrom.bin` (speech vocabulary) are optional.
3. Pick the `.uf2` for your board from the [table at the bottom of this page](#downloads___),
   hold **BOOTSEL** while connecting the board over USB, and copy the file to the drive that
   appears.

[Full setup instructions are in the readme](https://github.com/PicoPlus-devel/pico-994A#setup-overview).

> [!IMPORTANT]
> RP2350 only. The original Raspberry Pi Pico (RP2040) does not have enough memory for the
> TI-99/4A.

# v0.4

## What's new

- **Pico Computer 3.** The emulator now runs on the [Pico Computer 3](https://github.com/UKTailwind/Pico-Computer-3): HDMI, sound through HDMI or the audio jack, a USB keyboard and USB controllers on the USB-A ports, PSRAM, and NES or SNES controllers and a Wii Classic controller on the I/O header. The pin assignment may still change. See [Pico Computer 3](https://github.com/PicoPlus-devel/pico-994A#pico-computer-3). Thanks to @javavi.
- **PCB for the Waveshare RP2350-PiZero.** The emulator now runs on a PCB for the Waveshare RP2350-PiZero, designed by @szuping: HDMI, sound through HDMI or the audio jack, a USB keyboard or a USB controller on the PIO-USB port, NES and SNES controller ports, and a socket for an ESP32 module that connects Bluetooth controllers through [BlueRetro](https://github.com/darthcloud/blueretro). The PCB design is not yet available. See [PCB with Waveshare RP2350-PiZero](https://github.com/PicoPlus-devel/pico-994A#pcb-with-waveshare-rp2350-pizero).

## Changes

- **NES and SNES controllers on the controller ports are now read with the same timing as a real console**, on all boards. Thanks to @javavi.
- Under pico-bootLoader, the settings menu entry that goes back to the bootloader is now called **Return to boot loader**.

## Fixes

- **The picture no longer drops out in DVI mode.** On boards with HSTX video, with *Display Mode* set to DVI, the picture could randomly drop out.
- **No more flickering lines in DVI mode.** On boards with HSTX video, with *Display Mode* set to DVI, now and then a single line was shown shifted to the right for one frame.

# v0.3.1

## Changes

- **USB keyboard in the menus.** Enter selects and Esc goes back.

## Fixes

- **USB controllers and keyboards on the Adafruit Fruit Jam and other boards with PIO-USB are now found at every start.** On boards with an older RP2350 chip they were often not detected.

# v0.3

After updating, all settings return to their defaults once.

## What's new

- **Olimex RP2040-PICO-PC.** The emulator now runs on the [Olimex RP2040-PICO-PC](https://www.olimex.com/Products/MicroPython/PICO/RP2040-PICO-PC/) with a Raspberry Pi Pico 2: HDMI, sound through HDMI and the audio jack, a USB keyboard or controller on the USB-A port and a NES or SNES controller on the UEXT connector. See [Olimex RP2040-PICO-PC](https://github.com/PicoPlus-devel/pico-994A#olimex-rp2040-pico-pc). Contributed by [DnCraptor](https://github.com/DnCraptor).

## Fixes

- **The Controller Test screen is now closed by holding SELECT + UP** for 2 seconds. SELECT + START conflicted with some 8BitDo wireless controllers.
- **The B button of the AliExpress SNES USB controller works without pressing Y first.**

# v0.2

## What's new

- **Overscan fix in menu.** A new setting for TVs that cut off the edges of the screen. It leaves the top and bottom rows of the menus blank, and optionally the first and last columns as well. The change is shown right away in the settings menu.
- **More options on one page in the settings menu.** The color palette is now only shown while one of the menu color options is selected, which leaves room for more options on screen.
- **Quicker saving in the settings menu.** Press SELECT on any setting to jump straight to the SAVE/CANCEL/DEFAULT row.

## Fixes

- **Controller test screen** shows the controller outline and the list of controllers correctly again.

# v0.1

First release.

## What works

- **The complete console**: CPU, video and sound, with the 32K Memory Expansion always fitted.
- **Cartridges** as `.rpk` files or classic C/D/G `.bin` sets. The script `tools/mkrpk.py` in
  the repository converts `.bin` sets to `.rpk`.
- **TI BASIC** without a cartridge, through the `TI BASIC` entry in the menu.
- **USB keyboard** mapped onto the TI keyboard, including FCTN, CTRL and ALPHA LOCK. PC keys
  such as the arrows, Backspace and F1-F10 do what a TI user would expect.
- **Two joysticks** from USB gamepads, NES/SNES controllers or Wii controllers. SELECT and
  START type 1 and 2, so most games can be started without a keyboard.
- **Disk drives DSK1-DSK3** with `.dsk` images, read and write. Images next to a cartridge
  are inserted automatically; others can be inserted from the settings menu, where blank
  disks can also be created. Saved programs are written to the SD card straight away.
- **Speech Synthesizer.** Cartridges such as Parsec and Alpiner talk without extra files.
  `CALL SAY` in Extended BASIC uses the vocabulary in `spchrom.bin`.
- **Cassette (CS1/CS2).** `SAVE CS1` and `OLD CS1` with `.wav` or `.cas` files. Recordings of
  real cassettes load as they are.
- **Pasting BASIC listings over the serial port**, so a program written on a PC does not have
  to be typed in. Switch on **Serial keyboard** in the settings menu. Not available on the
  Murmulator boards or the Pimoroni Pico DV Demo Base.
- Builds for all 11 RP2350 hardware configurations, plus the
  [pico-bootLoader](https://github.com/PicoPlus-devel/pico-bootLoader) variant.

## Limitations

- The disk controller, the speech vocabulary used by `CALL SAY`, and cartridges larger than
  about 32 KB need a board with PSRAM.
- `CALL SAY` in Extended BASIC has not yet been tested on hardware.
- Not available yet: save states, SAMS memory expansion, p-code card.
- The emulation core is licensed for non-commercial use only. See
  [LICENSE](https://github.com/PicoPlus-devel/pico-994A/blob/main/LICENSE).

<a name="downloads___"></a>
## Downloads by configuration

Binaries for each configuration are listed below. For board-by-board wiring, PCB designs and
3D-printable cases, refer to the
[pico-infonesPlus documentation](https://github.com/PicoPlus-devel/pico-infonesPlus#setup). The
boards and their pinouts are the same, but pico-994A runs on RP2350 boards only.

### Standalone boards

| Board | Binary |
|:--|:--|
| Adafruit Metro RP2350 | [pico994A_AdafruitMetroRP2350_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_AdafruitMetroRP2350_arm.uf2) |
| Adafruit Fruit Jam | [pico994A_AdafruitFruitJam_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_AdafruitFruitJam_arm_piousb.uf2) |
| Adafruit Feather RP2350 + TLV320DAC3100 | [pico994A_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2) |
| Waveshare RP2350-PiZero | [pico994A_WaveShareRP2350PiZero_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_WaveShareRP2350PiZero_arm_piousb.uf2) |

### Breadboard / Custom PCB

Adafruit DVI Breakout + MicroSD card breakout, or the custom PCB.

| Board | Binary |
|:--|:--|
| Pico 2 / Pimoroni Pico Plus 2 | [pico994A_AdafruitDVISD_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_AdafruitDVISD_pico2_arm.uf2) |

### PCB Waveshare RP2350-Zero (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_WaveShareRP2350ZeroWithPCB_arm.uf2)

### PCB Waveshare RP2350-USBA (PCB required)

[Binary](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_WaveShare2350USBA_arm_piousb.uf2)

### Pimoroni Pico DV Demo Base

| Board | Binary |
|:--|:--|
| Pico 2 / Pimoroni Pico Plus 2 | [pico994A_PimoroniDVI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_PimoroniDVI_pico2_arm.uf2) |

### SpotPear HDMI

For more info about the SpotPear HDMI see https://spotpear.com/index/product/detail/id/1207.html and https://spotpear.com/index/study/detail/id/971.html.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico994A_SpotpearHDMI_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_SpotpearHDMI_pico2_arm.uf2) |

### Murmulator M1

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico994A_MurmulatorM1_pico2_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_MurmulatorM1_pico2_arm.uf2) |

### Murmulator M2

For more info about the Murmulator see https://murmulator.ru/.

| Board | Binary |
|:--|:--|
| Pico 2 / Pico 2 W | [pico994A_MurmulatorM2_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_MurmulatorM2_arm.uf2) |

### Olimex RP2040-PICO-PC

| Board | Binary |
|:--|:--|
| Pico 2 | [pico994A_OlimexPicoPC_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_OlimexPicoPC_arm.uf2) |

There is no Pico 2 W binary for this board.

### Pico Computer 3

For more info about the Pico Computer 3 see https://github.com/UKTailwind/Pico-Computer-3

| Board | Binary |
|:--|:--|
| Pico Computer 3 (PC3) | [pico994A_PicoComputer3_arm.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_PicoComputer3_arm.uf2) |

### PCB Waveshare RP2350-PiZero

For more info about this PCB, designed by Liu Szuping, see [PCB with Waveshare RP2350-PiZero](https://github.com/PicoPlus-devel/pico-infonesPlus#pcb-with-waveshare-rp2350-pizero). The PCB design is not yet available.

| Board | Binary |
|:--|:--|
| Waveshare RP2350-PiZero (*) | [pico994A_WaveShareRP2350PiZeroLiuPCB_arm_piousb.uf2](https://github.com/PicoPlus-devel/pico-994A/releases/latest/download/pico994A_WaveShareRP2350PiZeroLiuPCB_arm_piousb.uf2) |

This binary is only for the RP2350-PiZero on this PCB.

(*) If you fitted this board with PSRAM and it has a non-Winbond flash chip, apply the [flash_config fix](https://github.com/PicoPlus-devel/pico-infonesPlus#psram-with-a-non-winbond-flash-chip) before flashing the emulator.
