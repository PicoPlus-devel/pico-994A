# Sample cassette tapes

Three TI BASIC programs dumped from a real cassette, for testing `OLD CS1`. Copy them to
`/saves/ti99/tapes/` on the SD card, then from TI BASIC:

```
OLD CS1
```

Answer the rewind and play prompts with ENTER. The emulator picks the tape up from the
menu, or asks which one to load if the deck is empty.

| File | Length | Contents |
|------|--------|----------|
| `13 Bouncing Ball 1.wav` | 13s | the shortest, so the quickest thing to try first |
| `19 Kaleidoscope.wav` | 19s | a graphics demo - obvious when it has loaded |
| `31 Texman.wav` | 59s | a longer program, several records |

These are worth having as test material precisely because they are **not** tapes this
emulator wrote. They went through a real cassette recorder, so they carry the DC offset
and the wandering tape speed that the level detector exists to cope with - each of them
reads back a full 768-byte leader followed by the `>FF` marker, which a tape written here
cannot demonstrate because the encoder and the decoder would be the same code.

`tools/tapeinfo.py` reports what is on one, and
`TAPETEST_REAL='assets/tapes/19 Kaleidoscope.wav' make -C tools/tapetest run` puts it
through the emulator's own decoder on the host.

## Provenance

From <https://github.com/sonic2000gr/TI99> (side A), a collection of TI-99/4A programs
written and recorded by the repository's author between 1984 and 1987, published under the
BSD-2-Clause licence. Filenames are unchanged.

That repository has 40-odd more, and <http://ftp.whtech.com/Cassettes/> has a much larger
archive of commercial titles - those are 32-bit float WAVs, which load here too.
