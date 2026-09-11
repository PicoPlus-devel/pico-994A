# Host harness

`ti99_host` runs the TI-99/4A emulation core on Linux, without a board. It boots the
console, optionally with a cartridge, pastes a BASIC listing in through the same logic as
the board's serial keyboard, then types `RUN` and prints the screen.

It was written to find out why pasted listings lost characters. It is the quickest way to
check a listing, a change to the serial keyboard, or anything in the core that BASIC
exercises. A 4.5 KB listing takes five minutes to type on the board and under a second
here.

## Building

```sh
hosttest/build.sh              # -> hosttest/ti99_host
SANITIZE=1 hosttest/build.sh   # with AddressSanitizer
```

The console ROM and GROM are read from `bios/` in the repository, under the names used on
the SD card (`994aROM.bin`, `994aGROM.bin`; case does not matter). `spchrom.bin` is needed
for `CALL SAY` and `CALL SPGET`. `bios/` is not tracked.

## Running

```sh
hosttest/ti99_host [-t] [-o speech.wav] <cartridge.rpk | -> <listing.bas> [commands]
```

- A cartridge is selected from the second entry of the console menu, which for Extended
  BASIC is Extended BASIC. `-` instead of a cartridge boots the bare console and selects
  TI BASIC.
- The listing is framed as `tools/copybas.sh` frames it: five `PRINT 0` lines and `NEW`
  before it, five `PRINT 0` lines after.
- `commands` are typed once the listing is in. The default is `RUN\n\W`. Escapes:

  | Escape | Meaning            |
  |--------|--------------------|
  | `\n`   | ENTER              |
  | `\w`   | wait one second    |
  | `\W`   | wait ten seconds   |
  | `\d`   | print the screen   |
  | `\\`   | a backslash        |

- The screen is printed when BASIC has started and again at the end.
- `-o` writes everything spoken to an 8 kHz WAV, with 0.2 s of silence between
  utterances.

Examples:

```sh
# Paste the speech demo into Extended BASIC and run it
hosttest/ti99_host ~/roms/ti99/"Extended Basic.rpk" examples/speech.bas

# The same, then choose menu entry 7 and make one guess. The wait after 7 lets the game
# finish speaking its prompt: a key typed while CALL SAY is still talking can be missed.
hosttest/ti99_host ~/roms/ti99/"Extended Basic.rpk" examples/speech.bas 'RUN\n\W7\W50\n\w\w\w'

# Paste the character test into TI BASIC
hosttest/ti99_host - examples/pastetest.bas
```

## Output

- `LINE DAMAGED`: before each ENTER in the listing, the line being edited is read back
  from the screen and compared with the file. A mismatch prints both. This is the check to
  look at first when BASIC reports an error that makes no sense for the listing: a lost
  character in a line number renumbers the line, and the error then appears elsewhere.
- `speech:` gives the length of each utterance. A resident-vocabulary word decoded
  correctly runs its full length (HELLO is 625 ms); a wrongly decoded one stops early.
- The last line gives the number of keys typed, the emulated time, and how many lines were
  checked and damaged.
- `-t` prints, for the 1.5 s after every ENTER, the number of full keyboard sweeps in
  each frame (`+` for ten or more), with `P`, `R` and `I` where the next key was pressed,
  released and finished with. This is what showed that BASIC makes a single sweep of its
  own while busy after ENTER, while the editor polling for input makes two or more.

The exit status is 0 when every line arrived intact, 1 when the machine could not be
started, and 2 when a line was damaged.

## Limits

- The serial keyboard in `ti99_host.c` is a port of the one in `main.cpp`. A change to
  one has to be made to the other.
- Lines are only checked while the listing is being entered. Commands typed to a running
  program go through the same serial-keyboard rules, and, as on the board, a program
  reading the keyboard with `CALL KEY` may miss some of them.
- There is no video output beyond the screen text, and no disk or cassette.
