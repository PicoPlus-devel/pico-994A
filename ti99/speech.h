// =====================================================================================
// speech.h - TI-99/4A Solid State Speech Synthesizer.
//
// The module is a TMS5200 (TMC0285 / CD2501E) on early units and a TMS5220 on later
// ones, plus two TMS6100 serial ROMs holding the ~32K resident vocabulary. Both are
// LPC-10 synthesisers: a 10-stage lattice filter driven either by a pitch-controlled
// chirp (voiced) or by noise (unvoiced), with parameters interpolated across eight
// periods of every 25ms frame.
//
// Upstream DS994a does not synthesise speech - its speech.c fingerprints the first
// four bytes of each Speak External command and plays a pre-recorded sample through
// the DS sound library, for twelve known titles. That does not port, so this is a real
// implementation of the chip instead.
//
// Cost on RP2350: ~340 bytes of coefficient tables and roughly 160k multiply-accumulates
// per second at the chip's 8kHz rate - well under 1% of the CPU. The 32K vocabulary ROM
// (spchrom.bin) is only allocated when the file is present on the SD card.
// =====================================================================================
#ifndef _SPEECH_H_
#define _SPEECH_H_

#include "ti99_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

// Which coefficient set to use. The TI-99/4A shipped with the TMS5200; later modules
// used the TMS5220, whose pitch and K tables differ audibly.
enum
{
    SPEECH_CHIP_TMS5200 = 0,
    SPEECH_CHIP_TMS5220 = 1
};

// --- memory-mapped interface (called from the CPU core) -------------------------------
extern void SpeechDataWrite(u8 data);
extern u8   SpeechDataRead(void);
extern void SpeechInit(void);

// --- port interface -------------------------------------------------------------------
// Hand over the vocabulary ROM image. Pass NULL to run without one: Speak External still
// works (that is how nearly all cartridges do speech), only the resident vocabulary that
// CALL SAY reaches is unavailable. The module reports itself absent if never given one.
extern void SpeechSetROM(u8 *rom, u32 size);
extern void SpeechSetChip(u8 chipType);
extern u8   SpeechIsPresent(void);

// Is the chip currently producing sound? Lets the mixer skip the work when silent.
extern u8   SpeechIsTalking(void);

// Produce one sample at the chip's native 8kHz rate, roughly -8192..8191.
extern s16  SpeechGetSample(void);

// --- diagnostics ----------------------------------------------------------------------
// Built in with -DTI99_SPEECH_TRACE. The chip has a lot of ways to stay silent that all
// look identical from the speaker, so this counts what the CPU actually does to it -
// commands issued, bytes queued, whether talking ever started, whether the FIFO ran dry -
// and prints a summary line only when something changed. Printing per access would flood
// the port and wreck the frame timing, which is why it is counters and not a log.
#ifdef TI99_SPEECH_TRACE
extern void SpeechTraceTick(void);      // call once per emulated frame
#else
#define SpeechTraceTick() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
