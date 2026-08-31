// =====================================================================================
// cassette.h - TI-99/4A cassette port (CS1 / CS2).
//
// Written for this port; DS994a has no cassette support ("we don't have analog cassette
// support so you'll have to load from Disk", DS994a-README.md). The console DSR drives
// the tape directly through the TMS9901:
//
//   CRU 22 (w)  CS1 motor relay          CRU 24 (w)  audio gate (tape -> TV speaker)
//   CRU 23 (w)  CS2 motor relay          CRU 25 (w)  data out
//   CRU 27 (r)  data in
//
// There is no sector interface and no filesystem to intercept - the DSR bit-bangs the
// data line and times the bit cells against the 9901 interval timer, so the emulation
// has to work at the level of a signal in time. That is what the deck below models: one
// tape, positioned by CPU cycle count, running only while a motor bit is high.
//
// CS1 is read/write and CS2 is record-only on real hardware; both motors drive the same
// data lines, so one deck serves both and follows whichever motor is on.
//
// Two file formats:
//   .wav   the interop format. Any mono/stereo PCM rate, 8-bit unsigned or 16-bit
//          signed, so real cassette dumps load unchanged and saved tapes open on a PC.
//   .cas   the compact one. One bit per cell, bi-phase mark, no header.
// =====================================================================================
#ifndef _TI99_CASSETTE_H_
#define _TI99_CASSETTE_H_

#include "ti99_compat.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Where tapes live. Created on demand - FAT is case-insensitive, so this nests inside
// the /SAVES directory the framework already makes at boot.
#define TAPE_DIR            "/saves/ti99/tapes"
#define TAPE_MAX_NAME       25          // 24 characters plus the terminator
#define TAPE_MAX_LISTED     64          // tapes offered in the menu

// What the deck is doing. Mirrors the buttons on a real recorder.
typedef enum
{
    TAPE_EMPTY = 0,     // nothing loaded
    TAPE_PLAY,          // loaded for reading
    TAPE_RECORD_WAV,    // recording to a .wav
    TAPE_RECORD_CAS     // recording to a .cas
} TapeMode;

// --- lifecycle -----------------------------------------------------------------------
void cassette_init(void);           // called from ti99_reset()
void cassette_shutdown(void);       // closes the tape; called from ti99_free_memory()

// What the deck needs from the front end, discovered from the console's own CRU traffic.
// SAVE CS1 and OLD CS1 name no file - the TI cassette device takes only a device name -
// so the tape has to be chosen at the moment the console asks for it.
#define TAPE_REQ_NONE       0
#define TAPE_REQ_PLAY       1       // console wants to read and the deck is empty
#define TAPE_REQ_RECORD     2       // console has started writing and the deck is empty

// Polled once per frame by the front end, which puts up the prompt and then clears it.
int  cassette_pending_request(void);
void cassette_clear_request(void);

// --- CRU, called from tms9901.c ------------------------------------------------------
void cassette_cru_write(u8 pin, u8 dataBit);    // pins 22..25
u8   cassette_read_bit(void);                   // pin 27

// --- per-frame housekeeping, called from main.cpp -------------------------------------
// Prefetches the next block of a tape being read and flushes one being written. All SD
// access happens here so that a CRU read can never block on the card.
void cassette_frame_tick(void);

// --- audio-gate monitor, called from process_audio_frame() ---------------------------
// True while the DSR has the audio gate open over a running tape - that is how a real
// console lets you hear a load through the TV.
int  cassette_monitor_active(void);
// Fills `samples` mono frames at the emulator's output rate with the tape signal.
void cassette_monitor_fill(s16 *dest, int samples);

// --- menu interface ------------------------------------------------------------------
// The tape folder is rescanned on demand rather than watched; the menu calls
// cassette_refresh_list() when it opens.
void        cassette_refresh_list(void);
int         cassette_num_tapes(void);
const char *cassette_tape_name(int index);
int         cassette_selected(void);            // -1 when no tape is loaded
TapeMode    cassette_mode(void);

// Fills `buf` with the first unused TAPE-NN name, to pre-fill the label field.
void        cassette_default_name(char *buf, size_t bufsize);
// 1 if a tape of this name already exists in either format (drives the overwrite confirm).
int         cassette_name_exists(const char *name, TapeMode mode);

// Load `index` for playback, or start recording to `name`. Returns 0 on success.
// For the record modes `index` is ignored and `name` carries the label.
int         cassette_commit(int index, TapeMode mode, const char *name);
void        cassette_eject(void);
void        cassette_rewind(void);

#ifdef __cplusplus
}
#endif

#endif // _TI99_CASSETTE_H_
