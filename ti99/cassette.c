// =====================================================================================
// cassette.c - TI-99/4A cassette port (CS1 / CS2). See cassette.h for the CRU map.
//
// The console DSR bit-bangs the tape: it drives the data line through CRU 25, polls
// CRU 27 for the return, and times the bit cells against the 9901 interval timer. There
// is no sector call to trap, so this models the thing the DSR actually sees - a signal
// in time - and everything hangs off one idea:
//
//     the tape position is a function of the CPU cycle count, not of the frame loop.
//
// That is what makes the timing come out right. tms9900.cycles advances at 3MHz whatever
// the emulator is doing, the 9901 timer is derived from the same counter, so the DSR's
// stopwatch and the tape it is measuring cannot drift apart. It also costs nothing when
// no tape is loaded: no tick, no buffer, no allocation.
//
// tms9900.cycles is a free-running u32 that wraps every ~24 minutes at 3MHz, so every
// comparison here is on an unsigned delta and never on the absolute value.
// =====================================================================================

#include "ti99_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "ff.h"
#include "ti99_fileio.h"
#include "ti99.h"
#include "ti99_pico.h"      // TI99_AUDIO_SAMPLE_RATE, for the monitor's resample step
#include "cassette.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"

// -------------------------------------------------------------------------------------
// Timing
// -------------------------------------------------------------------------------------
// The 9900 in a TI-99/4A runs at 3MHz. This has to agree with the 9901 timer's 64
// clocks per tick, since the DSR calibrates one against the other.
#define TI99_CPU_HZ         3000000u

// Rate the recorder writes at. Matching the emulator's audio rate means the audio-gate
// monitor needs no resampling, and 44.1kHz puts a transition within ~23us of where it
// belongs - against a half cell of ~360us, that is far finer than the tape needs.
#define TAPE_WRITE_RATE     44100u

// TI cassette bit rate. The encoding is bi-phase mark: one transition at every cell
// boundary, plus one in the middle of the cell for a '1'.
//
// Measured off a tape the console itself wrote, with tools/tapeinfo.py: a cell is 768us,
// which is 2304 CPU cycles, or exactly 36 ticks of the 9901 timer the DSR is counting.
// That comes out at 1302 baud - the figure usually quoted, ~1379, is 6% out.
//
// It is still only a fallback. The .cas recorder calibrates itself from the leader and
// writes the period it measured into the file header, so playback uses the rate the
// machine actually wrote at; this constant only applies to a headerless .cas from
// somewhere else. Build with -DTI99_TAPE_TRACE to print the intervals as they are read.
#define TAPE_CELL_CYCLES    2304u
#define TAPE_BAUD           (TI99_CPU_HZ / TAPE_CELL_CYCLES)

#define TAPE_CELL_SAMPLES_Q16   ((u32)(((u64)TAPE_WRITE_RATE << 16) / TAPE_BAUD))
#define TAPE_HALFCELL_SAMPLES   (TAPE_WRITE_RATE / (TAPE_BAUD * 2))

// .cas header. Twelve bytes, then the packed bits MSB-first. The stored cell period is
// in TMS9900 cycles, which is machine time rather than sample time, so it stays correct
// whatever rate the file is later rendered at.
#define CAS_MAGIC           "TICAS1"
#define CAS_MAGIC_LEN       6
#define CAS_HEADER_LEN      12

// 8-bit unsigned levels written for the two logic states. ~50% of full scale, which is
// what a sanely recorded tape looks like and leaves room for the monitor mix.
#define TAPE_PCM_HIGH       0xC0
#define TAPE_PCM_LOW        0x40

// Buffers. 4KB at 44.1kHz is 93ms - about five frames of lookahead, so the per-frame
// prefetch is never in a hurry and a CRU read never waits on the card.
#define TAPE_IO_BUFFER      4096
// A .cas is tiny (~170 bytes per second of tape) so it is read whole rather than
// streamed. The cap is a sanity limit, not a real constraint.
#define TAPE_CAS_MAX        (64 * 1024)

// Runaway guard: a single emit_until() should never cover more than a frame or two.
#define TAPE_EMIT_CAP       TAPE_WRITE_RATE

// Most tape the detector will chew through in one call. Well over a frame's worth (735
// samples at 44.1kHz), so it keeps up without ever stalling on a long burst.
#define TAPE_DET_MAX        4096

// The monitor reads what the detector has already consumed rather than the file, so
// there is only ever one reader walking forward through the tape. Two readers at
// different positions in the same window would drag the file pointer back and forth and
// put SD reads inside CRU reads, which is exactly what the frame tick exists to avoid.
// Power of two: the index wraps by masking.
#define MON_RING            2048

// Samples averaged to seed the level detector's baseline. Two bit cells at 44.1kHz.
#define DET_PRIME_N         64

// -------------------------------------------------------------------------------------
// State
// -------------------------------------------------------------------------------------
typedef struct
{
    char name[TAPE_MAX_NAME];
    u8   isCas;
} TapeEntry;

static TapeEntry *tapeList;
static int        tapeCount;

static TapeMode  mode;
static int       selected = -1;         // index into tapeList, -1 when recording or empty
static TI99_FILE *fp;
static char      openPath[MAX_PATH];

// --- transport -----------------------------------------------------------------------
static u64 posQ32;              // tape sample position, 32.32
static u32 stepQ32;             // tape samples per CPU cycle, Q32
static u32 lastCycle;
static u8  motorOn;

// --- playback source -----------------------------------------------------------------
static u32 srcRate;             // sample rate of the mounted tape
static u32 srcFrameBytes;       // bytes per frame in the file (channels * width)
static u32 srcBits;             // 8, 16, 24 or 32
static u8  srcFloat;            // samples are IEEE floats rather than integers
static u32 srcDataStart;        // byte offset of the data chunk payload
static u32 srcDataBytes;        // payload size
static u32 srcFrames;           // total frames on the tape

static u8 *ioBuf;               // window into the file, or the synthesised .cas wave
static u32 ioFirstFrame;        // frame index of ioBuf[0]
static u32 ioFrames;            // valid frames in ioBuf
static u8  atEnd;               // ran off the end of the tape
static u8  reportedEnd;         // ... and said so, once

// --- .cas playback synthesiser -------------------------------------------------------
static u8 *casBits;             // whole file, when playing a .cas
static u32 casBitBytes;
static u32 casCellQ16;          // cell period in tape samples, Q16 - from the file header
static u32 casGenBit;           // next bit to lay down
static u8  casGenMidCell;       // next edge is a cell boundary (0) or mid-cell (1)
static u64 casGenEdgeQ16;       // sample position of the next edge, Q16
static u8  casGenLevel;

// --- level detector (playback) -------------------------------------------------------
static u32 detPos;              // frames already fed to the detector
static s32 detDcQ14;
static s32 detEnvQ14;
static u8  detLevel;
static u32 detPrimeCount;       // samples folded into the initial baseline estimate
static s32 detPrimeSum;

// --- recording -----------------------------------------------------------------------
static u32 wrEmitted;           // tape samples already written out
static u8  wrLevel;             // current state of CRU 25
static u32 wrCount;             // bytes buffered in ioBuf
static u32 wrPcmBytes;          // total PCM bytes in the file so far

static u32 casEdgePos;          // tape sample of the last transition
static u8  casHaveEdge;
static u32 casHalfCellQ8;       // running estimate, in tape samples, Q8
static u8  casShortPending;     // saw one half cell, waiting for its partner
static u32 casPackAccum;        // bits being packed, MSB first
static u8  casPackCount;

// --- what the console is asking for --------------------------------------------------
// The DSR does not announce what it is about to do, but its first CRU access says it: a
// write to the data line means a save, a read means a load. When that happens with an
// empty deck, the front end is asked to put a tape in - which is the moment the console
// is telling the user to press RECORD or PLAY anyway.
static u8 reqPending;
static u8 reqAsked;             // already asked during this motor-on pass - do not nag
// When the console last drove the data line, in CPU cycles. Used to tell a read that is
// part of a save - the DSR is free to glance at the input mid-write - from the read that
// starts the CHECK TAPE verify pass, which comes after the user has worked through two
// prompts. Measured in cycles rather than tape position because the motor may or may not
// be running in between, and in fact the DSR leaves it running right through the prompts.
static u32 lastWriteCycle;
static u8  haveWritten;

// Quiet time on the data line that means the save is over. A save writes at least one
// transition per bit cell (2304 cycles), so anything past ~65 cells is not a save any
// more; a human working through "PRESS CASSETTE STOP" takes seconds.
#define TAPE_WRITE_IDLE_CYCLES  (TI99_CPU_HZ / 20)      // 50ms

// Quiet time on the input that means a new load is starting rather than one continuing.
// The DSR polls at least once per half cell while it is reading, so gaps within a load
// are microseconds; the gap before an OLD CS1 is however long the user takes over
// "REWIND CASSETTE TAPE" and "PRESS CASSETTE PLAY".
#define TAPE_READ_IDLE_CYCLES   (TI99_CPU_HZ / 4)       // 250ms
static u32 lastReadCycle;
static u8  haveRead;

// --- monitor -------------------------------------------------------------------------
// Ring of the samples the detector has seen, plus a fractional read cursor that steps
// through it at the tape's rate while the caller pulls at the output rate.
static s16 monRing[MON_RING];
static u32 monRead;                 // absolute tape frame index to read next
static u32 monPhaseQ16;
static u32 monStepQ16;              // tape frames per output sample, Q16
static s16 monLast;

// =====================================================================================
// Small helpers
// =====================================================================================
static u16 le16(const u8 *p) { return (u16)p[0] | ((u16)p[1] << 8); }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void put32(u8 *p, u32 v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

static int name_is_cas(const char *n)
{
    const char *dot = strrchr(n, '.');
    return (dot && (strcasecmp(dot, ".cas") == 0));
}

static int name_is_tape(const char *n)
{
    const char *dot = strrchr(n, '.');
    return (dot && (strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".cas") == 0));
}

// Make sure /saves/ti99/tapes exists. ti99_mkdir() treats "already there" as success,
// so each level can just be asked for unconditionally.
static void ensure_tape_dir(void)
{
    ti99_mkdir("/saves");
    ti99_mkdir("/saves/ti99");
    ti99_mkdir(TAPE_DIR);
}

static void set_rate(u32 rate)
{
    srcRate = rate ? rate : TAPE_WRITE_RATE;
    stepQ32 = (u32)(((u64)srcRate << 32) / TI99_CPU_HZ);
    monStepQ16 = (u32)(((u64)srcRate << 16) / TI99_AUDIO_SAMPLE_RATE);
}

// =====================================================================================
// Transport
//
// Position advances only while a motor bit is high, so the DSR's "PRESS CASSETTE STOP
// THEN PRESS ENTER" pauses cost no tape, and a paused recorder writes no silence.
// lastCycle is updated either way, so a long stop does not get counted on restart.
// =====================================================================================
static inline void transport_advance(void)
{
    u32 now   = tms9900.cycles;
    u32 delta = (u32)(now - lastCycle);
    lastCycle = now;

    if (motorOn && mode != TAPE_EMPTY) posQ32 += (u64)delta * stepQ32;
}

static inline u32 transport_frame(void) { return (u32)(posQ32 >> 32); }

// =====================================================================================
// Playback - reading the tape
// =====================================================================================

// Pull the window of the file that contains `frame` into ioBuf. Sequential in practice,
// so this reads forward from the requested frame.
static int refill_from(u32 frame)
{
    if (!fp || !ioBuf) return 0;
    if (frame >= srcFrames) { atEnd = 1; return 0; }

    u32 maxFrames = TAPE_IO_BUFFER / srcFrameBytes;
    u32 want      = srcFrames - frame;
    if (want > maxFrames) want = maxFrames;

    if (ti99_fseek(fp, (long)(srcDataStart + frame * srcFrameBytes), SEEK_SET) != 0) return 0;

    size_t got = ti99_fread(ioBuf, srcFrameBytes, want, fp);
    ioFirstFrame = frame;
    ioFrames     = (u32)got;
    return (got > 0);
}

// One frame of the tape as a signed 16-bit value. Stereo takes the left channel - a
// cassette is mono and a stereo dump is the same signal twice (or data on one side).
static s32 source_sample(u32 frame)
{
    if (mode == TAPE_PLAY && casBits)
    {
        // .cas: the wave is synthesised into ioBuf, indexed the same way.
        if (frame < ioFirstFrame || frame >= ioFirstFrame + ioFrames) return 0;
        return (s32)((s16)((s16)ioBuf[frame - ioFirstFrame] - 128) << 8);
    }

    if (frame >= srcFrames) { atEnd = 1; return 0; }

    if (frame < ioFirstFrame || frame >= ioFirstFrame + ioFrames)
    {
        if (!refill_from(frame)) return 0;
        if (frame < ioFirstFrame || frame >= ioFirstFrame + ioFrames) return 0;
    }

    const u8 *p = ioBuf + (frame - ioFirstFrame) * srcFrameBytes;

    if (srcFloat)
    {
        // 32-bit IEEE float, nominally -1.0 to +1.0. Real cassette dumps turn up in this
        // format - it is what a PC recording program writes by default.
        union { u32 u; float f; } v;
        v.u = le32(p);
        float x = v.f * 32767.0f;
        if (x >  32767.0f) x =  32767.0f;
        if (x < -32768.0f) x = -32768.0f;
        return (s32)x;
    }

    switch (srcBits)
    {
        case 8:  return (s32)(((s16)p[0] - 128) << 8);          // 8-bit PCM is unsigned
        case 24: return ((s32)((p[2] << 24) | (p[1] << 16) | (p[0] << 8))) >> 16;
        case 32: return ((s32)le32(p)) >> 16;
        default: return (s32)(s16)le16(p);                      // 16-bit PCM is signed
    }
}

// -------------------------------------------------------------------------------------
// Level detector.
//
// A real recording arrives with a DC offset, some tilt, and an amplitude that depends on
// the deck, the tape and how the volume knob was set thirty years ago. So the threshold
// cannot be a constant: the baseline is tracked with the same shape of one-pole filter
// the PSG uses to remove its DC (sn76496_shim.c), in Q14 because at sample precision the
// feedback term has a fixed point it never leaves, and the switching threshold is a
// fraction of a running envelope estimate.
// -------------------------------------------------------------------------------------
static void detector_step(s32 s)
{
    // The one-pole filter below takes ~1024 samples to converge from zero, and on a
    // quietly recorded tape with a large DC offset the whole signal sits on one side of
    // the threshold until it gets there. So the baseline is primed from the mean of the
    // first few dozen samples instead.
    //
    // It has to be a mean and not a single sample: the tape is a square wave, so any one
    // sample sits at a peak rather than at the centre, and seeding from it would put the
    // baseline a full amplitude out - worse than starting at zero. DET_PRIME_N covers a
    // couple of bit cells at 44.1kHz and more at lower rates, and the leader the DSR
    // writes is thousands of cells long.
    if (detPrimeCount < DET_PRIME_N)
    {
        detPrimeSum += s;
        detPrimeCount++;
        detDcQ14 = (detPrimeSum / (s32)detPrimeCount) << 14;
    }
    else
    {
        detDcQ14 += (((s << 14) - detDcQ14) >> 10);      // ~7Hz corner at 44.1kHz
    }
    s32 ac = s - (detDcQ14 >> 14);

    s32 mag = (ac < 0) ? -ac : ac;
    s32 tgt = mag << 14;
    if (tgt > detEnvQ14) detEnvQ14 += ((tgt - detEnvQ14) >> 3);      // fast attack
    else                 detEnvQ14 += ((tgt - detEnvQ14) >> 11);     // slow decay

    s32 hyst = (detEnvQ14 >> 14) >> 3;                  // 12.5% of the envelope
    if (hyst < 64) hyst = 64;                           // floor, so silence cannot chatter

    if (detLevel) { if (ac < -hyst) detLevel = 0; }
    else          { if (ac >  hyst) detLevel = 1; }
}

// Walk the detector up to the current transport position. This is the tape's only
// reader: every frame is seen exactly once, in order, however unevenly the DSR polls.
// The monitor picks its audio out of monRing rather than going back to the file.
static void detector_advance(void)
{
    u32 target = transport_frame();

    // A synthesised .cas can only be read as far as it has been generated.
    if (casBits)
    {
        u32 end = ioFirstFrame + ioFrames;
        if ((s32)(target - end) > 0) target = end;
    }

    if ((s32)(target - detPos) <= 0) return;

    u32 count = target - detPos;
    if (count > TAPE_DET_MAX) count = TAPE_DET_MAX;      // bound the burst; we resume next call

    while (count--)
    {
        s32 s = source_sample(detPos);
        detector_step(s);
        monRing[detPos & (MON_RING - 1)] = (s16)s;
        detPos++;
    }
}

// =====================================================================================
// .cas playback - synthesise the waveform from the bits
//
// Bi-phase mark: an edge at every cell boundary, and one more in the middle of the cell
// when the bit is a '1'. Edge positions are kept in Q16 so the cell period does not have
// to be a whole number of samples.
// =====================================================================================
static u8 cas_next_bit(void)
{
    u32 byteIdx = casGenBit >> 3;
    if (byteIdx >= casBitBytes) return 0;
    u8 bit = (casBits[byteIdx] >> (7 - (casGenBit & 7))) & 1;
    casGenBit++;
    return bit;
}

// Pull the cell period out of the header, or fall back to the nominal rate for a file
// that has none. Returns the offset the packed bits start at.
static u32 cas_parse_header(const u8 *buf, u32 len)
{
    casCellQ16 = TAPE_CELL_SAMPLES_Q16;

    if (len < CAS_HEADER_LEN || memcmp(buf, CAS_MAGIC, CAS_MAGIC_LEN) != 0)
    {
        printf("[ti99] tape: .cas has no header, assuming %u baud\n", (unsigned)TAPE_BAUD);
        return 0;
    }

    u32 cellCycles = le32(buf + 8);
    if (cellCycles)
    {
        // Cycles -> tape samples at our rendering rate.
        casCellQ16 = (u32)(((u64)cellCycles * TAPE_WRITE_RATE << 16) / TI99_CPU_HZ);
        printf("[ti99] tape: .cas cell = %u cycles (%u baud)\n",
               (unsigned)cellCycles, (unsigned)(TI99_CPU_HZ / cellCycles));
    }
    return CAS_HEADER_LEN;
}

static void cas_synth_into(u8 *dest, u32 firstFrame, u32 count)
{
    for (u32 i = 0; i < count; i++)
    {
        u64 here = (u64)(firstFrame + i) << 16;
        while (here >= casGenEdgeQ16)
        {
            casGenLevel ^= 1;
            if (casGenMidCell)
            {
                // Mid-cell edge just happened; the next one closes the cell.
                casGenEdgeQ16 += casCellQ16 / 2;
                casGenMidCell = 0;
            }
            else
            {
                // Cell boundary. A '1' adds an edge halfway through.
                if (cas_next_bit())
                {
                    casGenEdgeQ16 += casCellQ16 / 2;
                    casGenMidCell = 1;
                }
                else
                {
                    casGenEdgeQ16 += casCellQ16;
                }
            }
        }
        dest[i] = casGenLevel ? TAPE_PCM_HIGH : TAPE_PCM_LOW;
    }

    if ((casGenBit >> 3) >= casBitBytes) atEnd = 1;
}

// Keep the synthesised window ahead of the detector. The generator is strictly
// sequential - it cannot be restarted from an arbitrary point - so the unconsumed tail is
// kept and new samples are appended behind it. A rewind resets the generator instead.
static void cas_keep_ahead(void)
{
    if ((s32)(detPos - ioFirstFrame) <= 0) return;

    u32 consumed = detPos - ioFirstFrame;
    if (consumed >= ioFrames)          consumed = ioFrames;
    if (consumed < TAPE_IO_BUFFER / 2) return;           // still plenty in hand

    u32 keep = ioFrames - consumed;
    if (keep) memmove(ioBuf, ioBuf + consumed, keep);

    // Carry on generating from where the old window ended.
    cas_synth_into(ioBuf + keep, ioFirstFrame + ioFrames, TAPE_IO_BUFFER - keep);
    ioFirstFrame = detPos;
    ioFrames     = TAPE_IO_BUFFER;
}

// =====================================================================================
// Recording - writing the tape
// =====================================================================================
static int flush_write_buffer(void)
{
    if (!fp || wrCount == 0) return 1;
    size_t wrote = ti99_fwrite(ioBuf, 1, wrCount, fp);
    int ok = (wrote == wrCount);
    if (!ok) printf("[ti99] tape: write failed (%u of %u bytes)\n", (unsigned)wrote, (unsigned)wrCount);
    wrPcmBytes += (u32)wrote;
    wrCount = 0;
    // The DSR gives no completion signal we could hook, so sync as we go: a power cut
    // mid-save then costs the tail of the tape rather than the whole file. Same reasoning
    // as the per-sector fflush in disk.c.
    ti99_fflush(fp);
    return ok;
}

static void write_byte(u8 b)
{
    ioBuf[wrCount++] = b;
    if (wrCount >= TAPE_IO_BUFFER) flush_write_buffer();
}

// Lay down PCM at the current level up to `target`.
static void wav_emit_until(u32 target)
{
    if ((s32)(target - wrEmitted) <= 0) return;

    u32 count = target - wrEmitted;
    if (count > TAPE_EMIT_CAP) count = TAPE_EMIT_CAP;    // bound the burst, resume next call

    u8 pcm = wrLevel ? TAPE_PCM_HIGH : TAPE_PCM_LOW;
    wrEmitted += count;                                  // only what actually got written
    while (count--) write_byte(pcm);
}

// -------------------------------------------------------------------------------------
// .cas recording. The interval between transitions says what was written: one half cell
// twice over is a '1', a whole cell is a '0'. The half-cell length is not assumed - it
// is taken from the intervals themselves, which the 768-byte leader supplies in
// abundance before any data arrives.
// -------------------------------------------------------------------------------------
static void cas_push_bit(u8 bit)
{
    casPackAccum = (casPackAccum << 1) | (bit & 1);
    if (++casPackCount == 8)
    {
        write_byte((u8)casPackAccum);
        casPackAccum = 0;
        casPackCount = 0;
    }
}

static void cas_record_edge(u32 pos)
{
    if (!casHaveEdge) { casEdgePos = pos; casHaveEdge = 1; return; }

    u32 interval = pos - casEdgePos;
    casEdgePos = pos;
    if (interval == 0) return;

#ifdef TI99_TAPE_TRACE
    printf("[ti99] tape edge: %u samples (half=%u.%02u)\n", (unsigned)interval,
           (unsigned)(casHalfCellQ8 >> 8), (unsigned)((casHalfCellQ8 & 0xFF) * 100 / 256));
#endif

    // Short means "about a half cell". Anything longer closes a whole cell.
    //
    // The estimate is Q8 and not whole samples for a reason: at sample precision the
    // feedback term floor-divides to zero as soon as the estimate is within 8 of the
    // truth, so it sticks a few percent off its starting guess and never calibrates at
    // all. The same fixed point bites the PSG DC blocker, which is why that one is Q14.
    u32 half = casHalfCellQ8 >> 8;
    if (interval < half + (half >> 1))
    {
        casHalfCellQ8 += (((s32)(interval << 8) - (s32)casHalfCellQ8) >> 3);
        if (casShortPending) { cas_push_bit(1); casShortPending = 0; }
        else                   casShortPending = 1;
    }
    else
    {
        cas_push_bit(0);
        casShortPending = 0;
    }
}

// =====================================================================================
// WAV header
// =====================================================================================
static void wav_write_header(u32 dataBytes)
{
    u8 h[44];
    memset(h, 0, sizeof(h));
    memcpy(h + 0,  "RIFF", 4);   put32(h + 4,  36 + dataBytes);
    memcpy(h + 8,  "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);   put32(h + 16, 16);
    h[20] = 1; h[21] = 0;                                   // PCM
    h[22] = 1; h[23] = 0;                                   // mono
    put32(h + 24, TAPE_WRITE_RATE);
    put32(h + 28, TAPE_WRITE_RATE);                         // byte rate = rate * 1 * 1
    h[32] = 1; h[33] = 0;                                   // block align
    h[34] = 8; h[35] = 0;                                   // bits per sample
    memcpy(h + 36, "data", 4);   put32(h + 40, dataBytes);

    ti99_fwrite(h, 1, sizeof(h), fp);
}

// Patch the two size fields once the length is known.
static void wav_finalise(void)
{
    u8 v[4];
    put32(v, 36 + wrPcmBytes);
    if (ti99_fseek(fp, 4, SEEK_SET) == 0) ti99_fwrite(v, 1, 4, fp);
    put32(v, wrPcmBytes);
    if (ti99_fseek(fp, 40, SEEK_SET) == 0) ti99_fwrite(v, 1, 4, fp);
    ti99_fflush(fp);
}

// -------------------------------------------------------------------------------------
// .cas header: magic, then the cell period in TMS9900 cycles. Written blank at the start
// of a recording and filled in on close, once the leader has been measured.
// -------------------------------------------------------------------------------------
static void cas_write_header(u32 cellCycles)
{
    u8 h[CAS_HEADER_LEN];
    memset(h, 0, sizeof(h));
    memcpy(h, CAS_MAGIC, CAS_MAGIC_LEN);
    put32(h + 8, cellCycles);
    ti99_fwrite(h, 1, sizeof(h), fp);
    wrPcmBytes += CAS_HEADER_LEN;
}

static void cas_finalise(void)
{
    // casHalfCell is in tape samples; store whole cells in CPU cycles so the number
    // means the same thing whatever rate the file is rendered at later.
    u32 cellCycles = (u32)(((u64)casHalfCellQ8 * 2 * TI99_CPU_HZ) / (TAPE_WRITE_RATE << 8));
    u8 v[4];
    put32(v, cellCycles);
    if (ti99_fseek(fp, 8, SEEK_SET) == 0) ti99_fwrite(v, 1, 4, fp);
    ti99_fflush(fp);
    printf("[ti99] tape: measured cell = %u cycles (%u baud)\n",
           (unsigned)cellCycles, (unsigned)(cellCycles ? TI99_CPU_HZ / cellCycles : 0));
}

// -------------------------------------------------------------------------------------
// Parse a WAV well enough to read a tape out of it. Deliberately permissive: real dumps
// turn up at every rate, 8 or 16 bit, mono or stereo, sometimes with a LIST chunk in
// front of the data. wavplayer.cpp cannot be reused here - it insists on 16/24-bit
// stereo and plays out rather than yielding samples.
// -------------------------------------------------------------------------------------
static int wav_parse(void)
{
    u8 hdr[256];
    if (ti99_fseek(fp, 0, SEEK_SET) != 0) return 0;
    size_t got = ti99_fread(hdr, 1, sizeof(hdr), fp);
    if (got < 44) return 0;

    if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return 0;

    u32 fmtOff = 0, fmtSize = 0, dataOff = 0, dataSize = 0;
    u32 p = 12;
    while (p + 8 <= got)
    {
        u32 sz = le32(&hdr[p + 4]);
        if      (memcmp(&hdr[p], "fmt ", 4) == 0) { fmtOff = p + 8; fmtSize = sz; }
        else if (memcmp(&hdr[p], "data", 4) == 0) { dataOff = p + 8; dataSize = sz; break; }
        p += 8 + sz + (sz & 1);                         // chunks are word aligned
    }
    if (!fmtOff || !dataOff) return 0;

    u16 format   = le16(&hdr[fmtOff + 0]);
    u16 channels = le16(&hdr[fmtOff + 2]);
    u32 rate     = le32(&hdr[fmtOff + 4]);
    u16 bits     = le16(&hdr[fmtOff + 14]);

    // WAVE_FORMAT_EXTENSIBLE carries the real tag in the first two bytes of its SubFormat
    // GUID, at offset 24 of the fmt chunk.
    if (format == 0xFFFE && fmtSize >= 40) format = le16(&hdr[fmtOff + 24]);

    // 1 is integer PCM, 3 is IEEE float. Dumps of real cassettes are commonly 32-bit
    // float at 48kHz, which is what a PC recording program writes without being asked.
    if (format != 1 && format != 3)       return 0;
    if (channels < 1 || channels > 2)     return 0;
    if (rate < 4000 || rate > 96000)      return 0;
    if (format == 3 && bits != 32)        return 0;
    if (format == 1 && bits != 8 && bits != 16 && bits != 24 && bits != 32) return 0;

    srcFloat      = (format == 3);
    srcBits       = bits;
    srcFrameBytes = channels * (bits / 8);
    srcDataStart  = dataOff;
    srcDataBytes  = dataSize;

    // Trust the file length over the header: truncated dumps are common.
    long fsize = ti99_file_size(openPath);
    if (fsize > 0 && (u32)fsize < dataOff + dataSize) srcDataBytes = (u32)fsize - dataOff;

    srcFrames = srcDataBytes / srcFrameBytes;
    set_rate(rate);

    printf("[ti99] tape: %s  %u Hz %u-bit %s %s, %u frames (%us)\n",
           openPath, (unsigned)rate, (unsigned)bits, srcFloat ? "float" : "PCM",
           channels == 2 ? "stereo" : "mono",
           (unsigned)srcFrames, (unsigned)(srcFrames / (rate ? rate : 1)));
    return (srcFrames > 0);
}

// =====================================================================================
// Mount / unmount
// =====================================================================================
static void reset_transport(void)
{
    posQ32      = 0;
    lastCycle   = tms9900.cycles;
    atEnd       = 0;
    reportedEnd = 0;

    ioFirstFrame = 0;
    ioFrames     = 0;

    detPos    = 0;
    detDcQ14  = 0;
    detEnvQ14 = 0;
    detLevel  = 0;
    detPrimeCount = 0;
    detPrimeSum   = 0;

    monRead     = 0;
    monPhaseQ16 = 0;
    monLast     = 0;
    memset(monRing, 0, sizeof(monRing));

    wrEmitted   = 0;
    wrCount     = 0;
    haveWritten = 0;
    haveRead    = 0;

    casGenBit     = 0;
    casGenMidCell = 0;
    casGenEdgeQ16 = 0;
    casGenLevel   = 0;

    casEdgePos      = 0;
    casHaveEdge     = 0;
    casHalfCellQ8   = TAPE_HALFCELL_SAMPLES << 8;
    casShortPending = 0;
    casPackAccum    = 0;
    casPackCount    = 0;
}

// Patch the header so what is on the card is a complete file, then put the write cursor
// back at the end so recording can carry on.
static void finalise_in_place(void)
{
    if (!fp) return;

    if      (mode == TAPE_RECORD_WAV) wav_finalise();
    else if (mode == TAPE_RECORD_CAS) cas_finalise();
    else return;

    ti99_fseek(fp, 0, SEEK_END);
}

static void close_tape(void)
{
    if (fp)
    {
        if (mode == TAPE_RECORD_WAV)
        {
            wav_emit_until(transport_frame());
            flush_write_buffer();
            wav_finalise();
            printf("[ti99] tape: saved %s (%u bytes PCM, %us)\n", openPath,
                   (unsigned)wrPcmBytes, (unsigned)(wrPcmBytes / TAPE_WRITE_RATE));
        }
        else if (mode == TAPE_RECORD_CAS)
        {
            if (casPackCount) { casPackAccum <<= (8 - casPackCount); write_byte((u8)casPackAccum); }
            flush_write_buffer();
            cas_finalise();
            printf("[ti99] tape: saved %s (%u bytes)\n", openPath, (unsigned)wrPcmBytes);
        }
        ti99_fclose(fp);
        fp = NULL;
    }

    if (ioBuf)   { free(ioBuf);   ioBuf = NULL; }
    if (casBits) { free(casBits); casBits = NULL; casBitBytes = 0; }

    mode        = TAPE_EMPTY;
    selected    = -1;
    openPath[0] = 0;
    wrPcmBytes  = 0;
    reset_transport();
}

// -------------------------------------------------------------------------------------
// BASIC's CHECK TAPE reads back what it just saved. On a real deck you rewind and press
// PLAY, which is exactly what the DSR has just asked the user to do - so do it for them
// rather than requiring a trip to the menu mid-prompt. Closing the recording finalises
// the file (WAV sizes, .cas cell period) so there is something to read.
// -------------------------------------------------------------------------------------
static int rewind_for_verify(void)
{
    char base[TAPE_MAX_NAME];
    const char *slash = strrchr(openPath, '/');
    snprintf(base, sizeof(base), "%s", slash ? slash + 1 : openPath);

    printf("[ti99] tape: switching to playback of %s to verify\n", base);

    close_tape();                       // flushes and finalises what we just recorded
    cassette_refresh_list();

    for (int i = 0; i < tapeCount; i++)
    {
        if (strcasecmp(tapeList[i].name, base) == 0)
            return cassette_commit(i, TAPE_PLAY, NULL);
    }

    printf("[ti99] tape: %s vanished before it could be verified\n", base);
    return -1;
}

// =====================================================================================
// CRU
// =====================================================================================
void cassette_cru_write(u8 pin, u8 dataBit)
{
    // The pin state is already stored by the caller; this only reacts to it.
    switch (pin)
    {
        case PIN_CS1_MOTOR:
        case PIN_CS2_MOTOR:
        {
            // Either motor runs the transport - CS1 and CS2 share the data lines.
            u8 nowOn = (tms9901.PinState[PIN_CS1_MOTOR] || tms9901.PinState[PIN_CS2_MOTOR]);
            if (nowOn == motorOn) break;

            transport_advance();            // account for the time up to the change
            motorOn = nowOn;
            reqAsked = 0;                   // a new pass may ask again

            if (mode == TAPE_EMPTY) break;

            if (motorOn)
            {
                // The recorder starts its clock where the tape is now, so a stop/start
                // cannot leave a gap of invented silence in the file.
                if (mode == TAPE_RECORD_WAV || mode == TAPE_RECORD_CAS)
                {
                    wrEmitted   = transport_frame();
                    casEdgePos  = wrEmitted;
                    casHaveEdge = 0;
                }
            }
            else if (mode == TAPE_RECORD_WAV || mode == TAPE_RECORD_CAS)
            {
                // "PRESS CASSETTE STOP" - leave the file valid on the card right now
                // rather than only when it is closed. Answer N to CHECK TAPE and the deck
                // stays armed indefinitely; without this the WAV would carry a zero-length
                // data chunk until the emulator was exited, and a power cut would leave a
                // file nothing could open. Same reasoning as the per-sector sync in disk.c.
                if (mode == TAPE_RECORD_WAV) wav_emit_until(transport_frame());
                flush_write_buffer();
                finalise_in_place();
            }
            break;
        }

        case PIN_AUDIO_GATE:
            // Nothing to do: the monitor mix reads the pin directly each frame.
            break;

        case PIN_TAPE_OUT:
        {
            // Track the level even with the motor stopped - the DSR sets the data line
            // before it starts the tape moving, and the first sample recorded has to be
            // the level the line was already sitting at, not the last one written.
            if (!motorOn) { wrLevel = dataBit; break; }

            transport_advance();
            u32 pos = transport_frame();

            lastWriteCycle = tms9900.cycles;
            haveWritten    = 1;

            if (mode == TAPE_RECORD_WAV)
            {
                wav_emit_until(pos);        // fill at the old level, then switch
                wrLevel = dataBit;
            }
            else if (mode == TAPE_RECORD_CAS)
            {
                if (dataBit != wrLevel) { cas_record_edge(pos); wrLevel = dataBit; }
            }
            else
            {
                // The console has started writing and there is no tape in the deck. Ask
                // for one; the leader is thousands of cells long, so the handful written
                // before the prompt appears costs nothing.
                if (mode == TAPE_EMPTY && !reqAsked) { reqPending = TAPE_REQ_RECORD; reqAsked = 1; }
                wrLevel = dataBit;          // still track it, for the monitor
            }
            break;
        }

        default:
            break;
    }
}

u8 cassette_read_bit(void)
{
    if (mode != TAPE_PLAY)
    {
        if (!motorOn) return 0;

        if (mode == TAPE_RECORD_WAV || mode == TAPE_RECORD_CAS)
        {
            // The console has stopped writing and started reading, which is BASIC's
            // CHECK TAPE verify pass. On a real deck the user rewinds and presses PLAY;
            // the DSR has just told them to, so do it rather than making them go and
            // find a menu. Closing the tape finalises the file we then read back.
            //
            // Only once the data line has gone quiet, though. The DSR is free to glance
            // at the input in the middle of a save, and turning the tape round on that
            // would truncate the recording where it happened.
            // Nothing written yet means this is not a verify pass at all - leave the
            // deck armed rather than turning round onto an empty file.
            if (!haveWritten) return 0;

            u32 quiet = (u32)(tms9900.cycles - lastWriteCycle);
            if (quiet < TAPE_WRITE_IDLE_CYCLES) return 0;

            printf("[ti99] tape: console read the input %ums after the last write\n",
                   (unsigned)(quiet / (TI99_CPU_HZ / 1000)));
            if (rewind_for_verify() != 0) return 0;
        }
        else
        {
            // Nothing loaded and the console wants data - ask for a tape.
            if (!reqAsked) { reqPending = TAPE_REQ_PLAY; reqAsked = 1; }
            return 0;
        }
    }

    // A load that is just starting winds the tape back. The console has told the user to
    // rewind and press play, and on a real deck they would have; here the tape has very
    // likely rolled since it was loaded, because the console leaves the motor relay
    // closed and the transport follows it - so by the time OLD CS1 is typed the tape is
    // sitting well past the data.
    //
    // "Just starting" means no read for a while, and that has to include never having
    // read at all: mounting a tape from the menu resets the read history, so keying only
    // off the gap left the very first read - the one that matters for a fresh load -
    // unable to rewind. Within a load the DSR polls every half cell, so the gap stays
    // in microseconds and this never fires mid-read.
    u32 now = tms9900.cycles;
    int newPass = (!haveRead) || ((u32)(now - lastReadCycle) > TAPE_READ_IDLE_CYCLES);
    if (newPass && transport_frame() != 0)
    {
        printf("[ti99] tape: load starting %u samples in - rewinding\n",
               (unsigned)transport_frame());
        cassette_rewind();
    }
    lastReadCycle = tms9900.cycles;
    haveRead      = 1;

    transport_advance();
    detector_advance();
    return detLevel;
}

int cassette_pending_request(void) { return reqPending; }
void cassette_clear_request(void)  { reqPending = 0; }

// =====================================================================================
// Per-frame housekeeping - the only place that touches the SD card
// =====================================================================================
void cassette_frame_tick(void)
{
    if (mode == TAPE_EMPTY) return;

    transport_advance();

    if (mode == TAPE_PLAY)
    {
        // Consume first, then top the window up - the slide is measured from the
        // detector, so it must not run ahead of it. The second pass picks up the tape the
        // refill just made available; doing it here as well as from the CRU read means a
        // burst of thousands of samples never lands inside a single poll.
        detector_advance();
        if (casBits) cas_keep_ahead();
        detector_advance();

        // Say so once when the tape runs out - "nothing happened" is otherwise
        // indistinguishable from a bad recording.
        if (atEnd && !reportedEnd)
        {
            reportedEnd = 1;
            printf("[ti99] tape: reached the end of %s - rewind to read it again\n", openPath);
        }
    }
    else if (motorOn)
    {
        if (mode == TAPE_RECORD_WAV) wav_emit_until(transport_frame());
        flush_write_buffer();
    }
}

// =====================================================================================
// Audio-gate monitor
//
// On a real console the DSR opens the audio gate during a load, which is why you hear
// the tape screech through the TV. Worth having: it is the only progress indication a
// cassette load gives you.
// =====================================================================================
int cassette_monitor_active(void)
{
    if (mode == TAPE_EMPTY || !motorOn) return 0;

    // Recording is always audible. A real console pipes the data tone to the TV during a
    // save as well as a load, and it is the only sign that anything is being written -
    // so this does not wait on the DSR having opened the audio gate.
    if (mode == TAPE_RECORD_WAV || mode == TAPE_RECORD_CAS) return 1;

    return (tms9901.PinState[PIN_AUDIO_GATE] != 0);
}

void cassette_monitor_fill(s16 *dest, int samples)
{
    if (mode == TAPE_RECORD_WAV || mode == TAPE_RECORD_CAS)
    {
        // Nothing to read back, so play what is being written. The level is only sampled
        // once per frame here, which is enough: the point is to hear that a save is
        // happening, and the recorded file is what actually matters.
        s16 v = wrLevel ? 16384 : -16384;
        for (int i = 0; i < samples; i++) dest[i] = v;
        return;
    }

    // Reader and writer are both locked to real time, so they track on average. This only
    // catches up after a hiccup, or the first frame after the motor starts.
    if ((s32)(detPos - monRead) > (s32)MON_RING || (s32)(monRead - detPos) > 0)
    {
        monRead = (detPos > MON_RING / 2) ? detPos - MON_RING / 2 : 0;
        monPhaseQ16 = 0;
    }

    for (int i = 0; i < samples; i++)
    {
        if ((s32)(detPos - monRead) > 0) monLast = monRing[monRead & (MON_RING - 1)];
        dest[i] = monLast;

        monPhaseQ16 += monStepQ16;
        monRead     += (monPhaseQ16 >> 16);
        monPhaseQ16 &= 0xFFFF;
    }
}

// =====================================================================================
// Menu interface
// =====================================================================================
void cassette_refresh_list(void)
{
    ensure_tape_dir();

    if (!tapeList)
    {
        tapeList = (TapeEntry *)malloc(sizeof(TapeEntry) * TAPE_MAX_LISTED);
        if (!tapeList) { tapeCount = 0; return; }
    }
    tapeCount = 0;

    DIR dir;
    if (f_opendir(&dir, TAPE_DIR) != FR_OK) return;

    FILINFO fno;
    while (tapeCount < TAPE_MAX_LISTED)
    {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        if (!name_is_tape(fno.fname)) continue;

        // Skip rather than truncate: a shortened name would list fine and then fail to
        // open, which is a worse outcome than not offering it.
        if (strlen(fno.fname) >= TAPE_MAX_NAME)
        {
            printf("[ti99] tape: ignoring %s (name longer than %d characters)\n",
                   fno.fname, TAPE_MAX_NAME - 1);
            continue;
        }

        TapeEntry *e = &tapeList[tapeCount++];
        snprintf(e->name, sizeof(e->name), "%s", fno.fname);
        e->isCas = name_is_cas(fno.fname) ? 1 : 0;
    }
    f_closedir(&dir);

    // Keep the currently loaded tape selected across a rescan.
    if (openPath[0] && mode == TAPE_PLAY)
    {
        const char *slash = strrchr(openPath, '/');
        const char *base  = slash ? slash + 1 : openPath;
        selected = -1;
        for (int i = 0; i < tapeCount; i++)
            if (strcasecmp(tapeList[i].name, base) == 0) { selected = i; break; }
    }
}

int cassette_num_tapes(void) { return tapeCount; }

const char *cassette_tape_name(int index)
{
    if (index < 0 || index >= tapeCount) return "";
    return tapeList[index].name;
}

int      cassette_selected(void) { return selected; }
TapeMode cassette_mode(void)     { return mode; }

void cassette_default_name(char *buf, size_t bufsize)
{
    ensure_tape_dir();

    for (int n = 1; n < 100; n++)
    {
        char cand[MAX_PATH];
        snprintf(cand, sizeof(cand), "%s/TAPE-%02d.wav", TAPE_DIR, n);
        if (ti99_file_exists(cand)) continue;
        snprintf(cand, sizeof(cand), "%s/TAPE-%02d.cas", TAPE_DIR, n);
        if (ti99_file_exists(cand)) continue;
        snprintf(buf, bufsize, "TAPE-%02d", n);
        return;
    }
    snprintf(buf, bufsize, "TAPE");
}

int cassette_name_exists(const char *name, TapeMode m)
{
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s.%s", TAPE_DIR, name,
             (m == TAPE_RECORD_CAS) ? "cas" : "wav");
    return ti99_file_exists(path) ? 1 : 0;
}

int cassette_commit(int index, TapeMode m, const char *name)
{
    close_tape();
    if (m == TAPE_EMPTY) return 0;

    ensure_tape_dir();

    ioBuf = (u8 *)malloc(TAPE_IO_BUFFER);
    if (!ioBuf) { printf("[ti99] tape: out of memory\n"); return -1; }

    if (m == TAPE_PLAY)
    {
        if (index < 0 || index >= tapeCount) { free(ioBuf); ioBuf = NULL; return -1; }
        snprintf(openPath, sizeof(openPath), "%s/%s", TAPE_DIR, tapeList[index].name);

        fp = ti99_fopen(openPath, "rb");
        if (!fp) { printf("[ti99] tape: cannot open %s\n", openPath); free(ioBuf); ioBuf = NULL; openPath[0] = 0; return -1; }

        if (tapeList[index].isCas)
        {
            long sz = ti99_file_size(openPath);
            if (sz <= 0 || sz > TAPE_CAS_MAX) { printf("[ti99] tape: bad .cas size %ld\n", sz); goto fail; }
            // A .cas is held in RAM whole, up to 64K of it. malloc panics rather than
            // returning NULL, so the "if (!raw)" below can never run - ask the heap
            // whether it can take this first.
            if ((u32)sz + 8192 > ti99_sram_free())
            {
                printf("[ti99] tape: %ldK .cas does not fit in free SRAM (%uK)\n",
                       sz / 1024, (unsigned)(ti99_sram_free() >> 10));
                goto fail;
            }
            u8 *raw = (u8 *)malloc((size_t)sz);
            if (!raw) goto fail;
            u32 rawLen = (u32)ti99_fread(raw, 1, (size_t)sz, fp);
            ti99_fclose(fp); fp = NULL;                 // held entirely in RAM from here

            u32 skip = cas_parse_header(raw, rawLen);
            casBitBytes = rawLen - skip;
            casBits = raw;
            if (skip) memmove(casBits, raw + skip, casBitBytes);

            set_rate(TAPE_WRITE_RATE);
            srcFrameBytes = 1;
            srcBits       = 8;
            srcFloat      = 0;
            srcFrames     = 0xFFFFFFFFu;                // generated on demand
            printf("[ti99] tape: %s  %u bytes, %u bits\n", openPath,
                   (unsigned)casBitBytes, (unsigned)(casBitBytes * 8));
        }
        else if (!wav_parse())
        {
            printf("[ti99] tape: %s is not a PCM WAV this can read\n", openPath);
            goto fail;
        }

        mode     = TAPE_PLAY;
        selected = index;
        reset_transport();

        // Prime the window now, while we are already in the menu and a card access costs
        // nothing, rather than on the first CRU poll after the motor starts.
        if (casBits) { cas_synth_into(ioBuf, 0, TAPE_IO_BUFFER); ioFirstFrame = 0; ioFrames = TAPE_IO_BUFFER; }
        else         { refill_from(0); }
        return 0;
    }

    // Recording.
    {
        char label[TAPE_MAX_NAME];
        if (name && name[0]) snprintf(label, sizeof(label), "%s", name);
        else                 cassette_default_name(label, sizeof(label));

        snprintf(openPath, sizeof(openPath), "%s/%s.%s", TAPE_DIR, label,
                 (m == TAPE_RECORD_CAS) ? "cas" : "wav");

        fp = ti99_fopen(openPath, "wb");
        if (!fp) { printf("[ti99] tape: cannot create %s\n", openPath); free(ioBuf); ioBuf = NULL; openPath[0] = 0; return -1; }

        mode       = m;
        selected   = -1;
        wrPcmBytes = 0;
        set_rate(TAPE_WRITE_RATE);
        srcFrameBytes = 1;
        srcBits       = 8;
        srcFloat      = 0;
        reset_transport();

        // Both formats get a placeholder header now, patched when the tape closes: the
        // WAV needs its sizes, the .cas needs the cell period we are about to measure.
        if (m == TAPE_RECORD_WAV) wav_write_header(0);
        else                      cas_write_header(0);

        printf("[ti99] tape: recording to %s\n", openPath);
        return 0;
    }

fail:
    if (fp) { ti99_fclose(fp); fp = NULL; }
    if (ioBuf)   { free(ioBuf);   ioBuf = NULL; }
    if (casBits) { free(casBits); casBits = NULL; casBitBytes = 0; }
    openPath[0] = 0;
    return -1;
}

void cassette_eject(void)
{
    close_tape();
    // Ejecting answers any outstanding "which tape?" - dropping it here stops a stale
    // request surfacing as a prompt several seconds after the console asked.
    reqPending = TAPE_REQ_NONE;
    reqAsked   = 0;
}

void cassette_rewind(void)
{
    if (mode != TAPE_PLAY) return;

    // reset_transport() puts the .cas generator back to bit 0 as well, which is what a
    // rewind has to mean for it - the generator only runs forwards.
    reset_transport();
    if (casBits) { cas_synth_into(ioBuf, 0, TAPE_IO_BUFFER); ioFirstFrame = 0; ioFrames = TAPE_IO_BUFFER; }
    else         { refill_from(0); }

    printf("[ti99] tape: rewound %s\n", openPath);
}

// =====================================================================================
// Lifecycle
// =====================================================================================
void cassette_init(void)
{
    // Called on every machine reset. A reset does not eject the tape - the deck sits on
    // the desk, it is not part of the console - but it does stop the motor, because the
    // 9901 pins are all cleared by TMS9901_Reset().
    motorOn    = 0;
    lastCycle  = tms9900.cycles;
    reqPending = TAPE_REQ_NONE;
    reqAsked   = 0;
    haveWritten = 0;
    haveRead    = 0;
}

void cassette_shutdown(void)
{
    close_tape();
    if (tapeList) { free(tapeList); tapeList = NULL; }
    tapeCount = 0;
    motorOn   = 0;
}
