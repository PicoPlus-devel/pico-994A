// Tests for pico-994A cassette support, driving the real cassette.c and tms9901.c.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdlib.h>

#include "ti99_compat.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cassette.h"
extern long ti99_file_size(const char *path);

#define TI99_CPU_HZ  3000000u
#define TAPE_RATE    44100u
#define TAPE_BAUD    1379u

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { \
    failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

// For assertions inside a loop: count once, and only report the first failure.
#define CHECK_ONCE(cond, ...) do { static int said = 0; if (!(cond) && !said) { \
    said = 1; checks++; failures++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

// ------------------------------------------------------------------ CRU helpers
// The CPU passes CRU bit numbers already shifted down, so these are what
// SBO/SBZ/TB actually hand to the 9901.
static void sbo(u16 bit) { TMS9901_WriteCRU(bit, 1, 1); }
static void sbz(u16 bit) { TMS9901_WriteCRU(bit, 0, 1); }
static u8   tb (u16 bit) { return TMS9901_ReadCRU(bit, 1) & 1; }

static void burn(u32 cycles) { tms9900.cycles += cycles; }

#define CELL_CYCLES   (TI99_CPU_HZ / TAPE_BAUD)         // 2175
#define HALF_CYCLES   (CELL_CYCLES / 2)


// =====================================================================================
// 1. TMS9901 timer: is it a real 3MHz/64 stopwatch now?
// =====================================================================================
// The DSR reads the decrementer by dropping into clock mode, which latches it.
static u32 read_timer(void)
{
    sbo(0);                                 // clock mode - latches the count
    u32 v = TMS9901_ReadCRU(1, 14) & 0x3FFF;
    sbz(0);                                 // back to I/O mode
    return v;
}

static void test_timer(void)
{
    printf("TMS9901 timer\n");

    TMS9901_Reset();
    tms9900.cycles = 12345;                 // deliberately not zero

    // Program the full 14-bit span so nothing wraps during the test.
    sbo(0);
    TMS9901_WriteCRU(1, 0x3FFF, 14);
    sbz(0);

    // Elapsed ticks must be cycles/64. Check a spread of intervals, including ones
    // that are not multiples of 64 - the remainder has to be carried, not dropped.
    struct { u32 cycles; u32 expect; } cases[] = {
        { 64,    1 }, { 63,    0 }, { 65,    1 },
        { 191,   2 }, { 6400,  100 }, { 50042, 782 },
    };
    u32 running = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        burn(cases[i].cycles);
        running += cases[i].cycles;
        u32 got = 0x3FFF - read_timer();
        CHECK(got == running / 64, "after %u cycles: %u ticks elapsed, expected %u",
              running, got, running / 64);
    }

    // A tape bit cell is ~700us. Resolution has to be far finer than that, or the
    // DSR cannot tell a half cell from a whole one. Upstream quantised to a 191-cycle
    // scanline in steps of 3; this should resolve a single 64-cycle tick.
    TMS9901_Reset();
    sbo(0); TMS9901_WriteCRU(1, 0x3FFF, 14); sbz(0);
    u32 a = read_timer();
    burn(64);
    u32 b = read_timer();
    CHECK(a - b == 1, "one 64-cycle tick should move the counter by exactly 1, moved %u", a - b);

    // Frozen while in clock mode: two reads with time passing in between must agree,
    // because entering clock mode latched the value.
    TMS9901_Reset();
    sbo(0); TMS9901_WriteCRU(1, 0x3FFF, 14); sbz(0);
    burn(1000);
    sbo(0);
    u32 f1 = TMS9901_ReadCRU(1, 14) & 0x3FFF;
    burn(6400);
    u32 f2 = TMS9901_ReadCRU(1, 14) & 0x3FFF;
    CHECK(f1 == f2, "clock mode should latch: %u then %u", f1, f2);
    sbz(0);

    // And the counter reloads rather than sticking at zero.
    TMS9901_Reset();
    sbo(0); TMS9901_WriteCRU(1, 100, 14); sbz(0);
    burn(64 * 101);                         // exactly one full pass
    u32 w = read_timer();
    CHECK(w == 100, "after one full pass of a 100-tick timer, expected 100, got %u", w);
    burn(64 * 30);
    w = read_timer();
    CHECK(w == 70, "30 ticks into the second pass, expected 70, got %u", w);
}

// =====================================================================================
// 2. CRU decode: the cassette bits must reach the deck, and bit 27 must not come back
//    through the keyboard alias
// =====================================================================================
static void test_cru_decode(void)
{
    printf("TMS9901 CRU decode\n");

    TMS9901_Reset();

    // The bug this replaces: bit 27 aliased to 19 (PIN_COL2), so driving the keyboard
    // column select changed what a tape read returned.
    sbz(27 - 8);                            // whatever the old alias pointed at
    sbo(PIN_COL2);                          // drive the column bit high
    u8 withColHigh = tb(PIN_TAPE_IN);
    sbz(PIN_COL2);
    u8 withColLow = tb(PIN_TAPE_IN);
    CHECK(withColHigh == withColLow,
          "tape input must not follow the keyboard column select (%u vs %u)",
          withColHigh, withColLow);

    // Motor and gate bits read back as written - the DSR checks them.
    sbo(PIN_CS1_MOTOR);
    CHECK(tb(PIN_CS1_MOTOR) == 1, "CS1 motor should read back set");
    sbz(PIN_CS1_MOTOR);
    CHECK(tb(PIN_CS1_MOTOR) == 0, "CS1 motor should read back clear");
    sbo(PIN_AUDIO_GATE);
    CHECK(tb(PIN_AUDIO_GATE) == 1, "audio gate should read back set");
    sbz(PIN_AUDIO_GATE);

    // Keyboard reads must be untouched by the alias-table change. Row 0 column 0 is
    // the '=' key; pressing it should pull bit 3 low (the 9901 reads active low).
    memset(tms9901.Keyboard, 0, sizeof(tms9901.Keyboard));
    sbz(PIN_COL1); sbz(PIN_COL2); sbz(PIN_COL3);
    CHECK(tb(3) == 1, "unpressed key should read 1");
    tms9901.Keyboard[TMS_KEY_EQUALS] = 1;
    CHECK(tb(3) == 0, "pressed '=' should read 0 on row 0");
    tms9901.Keyboard[TMS_KEY_EQUALS] = 0;

    // A multi-bit STCR reaching into the aliased region used to walk the wrong
    // addresses from the second bit on, because the alias overwrote the loop variable.
    // Bits 22..25 are all loopback, so a 4-bit read starting at 22 must return exactly
    // what was written there.
    sbo(22); sbz(23); sbo(24); sbz(25);
    u16 got = TMS9901_ReadCRU(22, 4);
    CHECK((got & 0x0F) == 0x05, "4-bit read at 22 expected 0b0101, got 0b%u%u%u%u",
          (got >> 3) & 1, (got >> 2) & 1, (got >> 1) & 1, got & 1);

    // Bit 22 is the CS1 motor - leave it running and the next test inherits a deck that
    // thinks the console is mid-save.
    sbz(22);
    sbz(24);
    cassette_eject();
}

// The cassette DSR times its bit cells against the 9901 timer, so it is in clock mode
// while it drives the data line. On the real chip a write above pin 15 does two things:
// it drops the chip out of clock mode, and it still lands on the pin. Upstream only did
// the first, so every data bit of a SAVE was discarded and the tape came out blank.
static void test_write_from_clock_mode(void)
{
    printf("CRU writes from clock mode\n");

    TMS9901_Reset();
    cassette_init();
    cassette_eject();

    // Program the timer and stay in clock mode, the way the DSR does mid-save.
    sbo(0);
    TMS9901_WriteCRU(1, 200, 14);

    // Now drive the motor from inside clock mode.
    sbo(PIN_CS1_MOTOR);
    CHECK(tms9901.PinState[PIN_TIMER_OR_IO] == IO_MODE,
          "a write above pin 15 should drop out of clock mode");
    CHECK(tms9901.PinState[PIN_CS1_MOTOR] == 1,
          "a write above pin 15 from clock mode must still reach the pin");

    // And the data line, which is the one that actually mattered.
    sbo(0);
    sbo(PIN_TAPE_OUT);
    CHECK(tms9901.PinState[PIN_TAPE_OUT] == 1, "tape-out written from clock mode was lost");

    sbo(0);
    sbz(PIN_TAPE_OUT);
    CHECK(tms9901.PinState[PIN_TAPE_OUT] == 0, "tape-out cleared from clock mode was lost");

    // The deck has to see those writes too, not just the pin latch. Record a pattern
    // where every single data write is issued from clock mode, exactly as the DSR does,
    // and check something lands on the tape.
    CHECK(cassette_commit(-1, TAPE_RECORD_WAV, "CLOCKMODE") == 0, "could not arm recording");
    sbo(PIN_CS1_MOTOR);

    u8 level = 0;
    for (int i = 0; i < 64; i++)
    {
        sbo(0);                                     // into clock mode, as the DSR would
        TMS9901_WriteCRU(1, 100, 14);               // reload the stopwatch
        level ^= 1;
        if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        burn(CELL_CYCLES);
    }
    burn(CELL_CYCLES * 4);
    sbz(PIN_CS1_MOTOR);
    cassette_eject();

    // Counting bytes is not enough: a stuck data line still fills the file with PCM at
    // whatever level it froze at, which is precisely how a blank tape passes for a
    // recording. Count the transitions instead.
    FILE *f = fopen("sandbox/CLOCKMODE.wav", "rb");
    CHECK(f != NULL, "no CLOCKMODE.wav written");
    if (!f) return;

    fseek(f, 44, SEEK_SET);
    int edges = 0, prev = -1, c;
    while ((c = fgetc(f)) != EOF)
    {
        int level = (c > 128) ? 1 : 0;
        if (prev >= 0 && level != prev) edges++;
        prev = level;
    }
    fclose(f);

    // 64 cells were written, so 64 transitions give or take the ends.
    CHECK(edges > 50, "recording driven from clock mode holds only %d transitions - the "
                      "data bits were swallowed and the tape is blank", edges);
}

// The mirror of the write case: the DSR polls the tape input from inside clock mode too,
// because that is where it is timing the bit cell from. Clock mode only redefines CRU
// bits 0-15; 16-31 stay the I/O pins. Upstream ran the clock decode over all 32, so a
// read of bit 27 returned a bit of the timer register and the tape was never seen.
static void record_bits(const u8 *bits, int nbits, TapeMode m, const char *label);

static void test_read_from_clock_mode(void)
{
    printf("CRU reads from clock mode\n");

    TMS9901_Reset();
    cassette_init();
    cassette_eject();

    // Record a tape with real transitions on it. A parked level will not do: the detector
    // is AC coupled, like the hardware, so a constant reads as nothing either way and the
    // test would pass whether the decode is right or not.
    u8 bits[128];
    for (int i = 0; i < 128; i++) bits[i] = (i >> 2) & 1;
    record_bits(bits, 128, TAPE_RECORD_WAV, "RDCLOCK");

    cassette_refresh_list();
    int idx = -1;
    for (int i = 0; i < cassette_num_tapes(); i++)
        if (strncmp(cassette_tape_name(i), "RDCLOCK", 7) == 0) idx = i;
    CHECK(idx >= 0, "RDCLOCK.wav was not listed");
    if (idx < 0) return;

    CHECK(cassette_commit(idx, TAPE_PLAY, NULL) == 0, "could not open RDCLOCK for playback");
    sbo(PIN_CS1_MOTOR);

    // Poll the tape the way the DSR does - from inside clock mode, with the timer loaded -
    // and compare against the same read taken in I/O mode at the same instant. They have
    // to agree on every sample, and the level has to actually move.
    int mismatches = 0, ones = 0, zeros = 0;
    for (int i = 0; i < 2000; i++)
    {
        burn(HALF_CYCLES / 4);
        cassette_frame_tick();

        sbz(0);                                     // I/O mode
        u8 io = tb(PIN_TAPE_IN);

        sbo(0);                                     // clock mode, timer full of ones
        TMS9901_WriteCRU(1, 0x3FFF, 14);
        u8 clk = tb(PIN_TAPE_IN);
        sbz(0);

        if (io != clk) mismatches++;
        if (io) ones++; else zeros++;
    }

    CHECK(ones > 100 && zeros > 100,
          "the tape did not toggle (%d high, %d low) - test cannot tell the paths apart",
          ones, zeros);
    CHECK(mismatches == 0,
          "%d of 2000 tape reads differed between I/O and clock mode - the clock decode "
          "is swallowing bit 27", mismatches);

    sbz(PIN_CS1_MOTOR);
    cassette_eject();
}

// =====================================================================================
// 3. Bi-phase mark round trip through the real code
//
// Writes a known bit pattern out through CRU 25 with correct cell timing, closes the
// tape, then plays it back and decodes CRU 27 the way the DSR does - by timing the gaps
// between transitions. What comes back has to be what went in.
// =====================================================================================
// The recovered stream can start a bit or two into the leader: the detector makes no
// level decisions until its baseline has settled. That is inherent and harmless - a real
// tape opens with a 768-byte leader and the DSR syncs on the >FF marker, not on a bit
// count. So align the way the DSR would, then demand an exact match from there on.
static int find_alignment(const u8 *wrote, int nwrote, const u8 *got, int ngot)
{
    for (int off = 0; off <= 8; off++)
    {
        // The final cell is not decidable: bi-phase mark needs the edge that closes a
        // cell to classify it, and the last one on the tape has no successor. Leave the
        // last couple of bits out of the comparison rather than pretend otherwise.
        int compare = (ngot < nwrote - off) ? ngot : nwrote - off;
        compare -= 2;
        if (compare < nwrote / 2) break;
        int ok = 1;
        for (int i = 0; i < compare; i++)
            if (got[i] != wrote[i + off]) { ok = 0; break; }
        if (ok) return off;
    }
    return -1;
}

static void record_bits(const u8 *bits, int nbits, TapeMode m, const char *label)
{
    CHECK(cassette_commit(-1, m, label) == 0, "could not start recording %s", label);

    sbo(PIN_CS1_MOTOR);                     // motor on
    u8 level = 0;
    sbz(PIN_TAPE_OUT);

    // Bi-phase mark: flip at every cell boundary, and again mid-cell for a '1'.
    for (int i = 0; i < nbits; i++)
    {
        level ^= 1;
        if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        if (bits[i])
        {
            burn(HALF_CYCLES);
            level ^= 1;
            if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
            burn(CELL_CYCLES - HALF_CYCLES);
        }
        else
        {
            burn(CELL_CYCLES);
        }
    }
    burn(CELL_CYCLES * 4);                  // let the tail flush
    sbz(PIN_CS1_MOTOR);                     // motor off
    cassette_eject();                       // closes and finalises the file
}

// Decode by timing transitions on CRU 27, which is what the console DSR does.
static int playback_bits(const char *name, u8 *out, int maxbits)
{
    cassette_refresh_list();
    int idx = -1;
    for (int i = 0; i < cassette_num_tapes(); i++)
        if (strncmp(cassette_tape_name(i), name, strlen(name)) == 0) idx = i;
    if (idx < 0) { printf("  FAIL tape %s not listed\n", name); failures++; return 0; }

    CHECK(cassette_commit(idx, TAPE_PLAY, NULL) == 0, "could not open %s for playback", name);

    sbo(PIN_CS1_MOTOR);

    // Poll finely enough to see every edge: a quarter of a half-cell.
    const u32 poll = HALF_CYCLES / 4;
    u8 prev = tb(PIN_TAPE_IN);
    u32 sinceEdge = 0;
    int nbits = 0;
    int shortPending = 0;
    u32 halfEst = HALF_CYCLES;
    int idleFor = 0;

    while (nbits < maxbits && idleFor < 200)
    {
        burn(poll);
        cassette_frame_tick();              // stands in for the per-frame housekeeping
        sinceEdge += poll;

        u8 now = tb(PIN_TAPE_IN);
        if (now == prev) { idleFor++; continue; }

        prev = now;
        idleFor = 0;
        u32 gap = sinceEdge;
        sinceEdge = 0;

        if (gap < halfEst + halfEst / 2)
        {
            halfEst = (halfEst * 7 + gap) / 8;
            if (shortPending) { out[nbits++] = 1; shortPending = 0; }
            else shortPending = 1;
        }
        else
        {
            out[nbits++] = 0;
            shortPending = 0;
        }
    }

    sbz(PIN_CS1_MOTOR);
    cassette_eject();
    return nbits;
}

static void test_roundtrip(TapeMode m, const char *fmt)
{
    printf("Round trip via %s\n", fmt);

    // A leader of zeros (as the real DSR writes) then a pattern with every transition
    // type: 0->1, 1->0, runs of each.
    u8 bits[256];
    int n = 0;
    for (int i = 0; i < 64; i++) bits[n++] = 0;          // leader
    static const u8 pattern[] = {1,1,1,1,1,1,1,1,        // 0xFF sync
                                 1,0,1,0,1,1,0,0,
                                 0,0,0,1,1,1,0,1,
                                 1,1,0,0,1,0,1,0};
    for (size_t i = 0; i < sizeof(pattern); i++) bits[n++] = pattern[i];

    char label[32];
    snprintf(label, sizeof(label), "RT%s", fmt);
    record_bits(bits, n, m, label);

    u8 got[512];
    int gotn = playback_bits(label, got, n + 8);

    int off = find_alignment(bits, n, got, gotn);
    CHECK(off >= 0, "recovered %d of %d bits, but no alignment matches exactly", gotn, n);
    CHECK(off <= 4, "alignment slipped %d bits - more than the baseline transient", off);

    if (off < 0)
    {
        printf("    wrote: "); for (int i = 60; i < n && i < 96; i++) printf("%u", bits[i]);
        printf("\n    read:  "); for (int i = 60; i < gotn && i < 96; i++) printf("%u", got[i]);
        printf("\n");
    }
    else
    {
        int usable = (gotn < n - off) ? gotn : n - off;
        CHECK(usable > n - 16, "only %d of %d bits usable after alignment", usable, n);
    }
}

// =====================================================================================
// 4. The transport must not move while the motor is off
// =====================================================================================
// The .cas recorder measures the cell period off the tape and stores it in the header.
// If that estimator does not converge, .cas playback runs at the wrong speed - and an
// integer filter that floor-divides to zero will sit on its initial guess for ever and
// look like it is working.
static void test_cas_calibration(void)
{
    printf(".cas rate calibration\n");

    u8 bits[512];
    int n = 0;
    for (int i = 0; i < 64; i++) bits[n++] = 0;              // leader
    for (int i = 0; i < 128; i++) bits[n++] = 1;             // plenty of half cells to learn from

    record_bits(bits, n, TAPE_RECORD_CAS, "CAL");

    FILE *f = fopen("sandbox/CAL.cas", "rb");
    CHECK(f != NULL, "no CAL.cas written");
    if (!f) return;

    unsigned char h[12];
    size_t rd = fread(h, 1, sizeof(h), f);
    fclose(f);
    CHECK(rd == sizeof(h), "CAL.cas is too short for a header");
    CHECK(memcmp(h, "TICAS1", 6) == 0, "CAL.cas has no TICAS1 magic");

    u32 cell = h[8] | (h[9] << 8) | (h[10] << 16) | (h[11] << 24);
    u32 baud = cell ? (TI99_CPU_HZ / cell) : 0;

    // The test writes at exactly CELL_CYCLES. Allow 2%: the estimator is a one-pole
    // filter, not an exact measurement.
    u32 lo = CELL_CYCLES - CELL_CYCLES / 50, hi = CELL_CYCLES + CELL_CYCLES / 50;
    CHECK(cell >= lo && cell <= hi,
          "measured cell %u cycles (%u baud), wrote %u cycles (%u baud) - calibration did not converge",
          cell, baud, (u32)CELL_CYCLES, TI99_CPU_HZ / (u32)CELL_CYCLES);
}

static void test_motor_gate(void)
{
    printf("Motor gating\n");

    u8 bits[64];
    for (int i = 0; i < 64; i++) bits[i] = (i & 1);
    record_bits(bits, 64, TAPE_RECORD_WAV, "MOTOR");

    long withMotor = ti99_file_size("/saves/ti99/tapes/MOTOR.wav");

    // Same again, but with a long motor-off pause in the middle. The file must not grow:
    // a stopped tape records nothing.
    CHECK(cassette_commit(-1, TAPE_RECORD_WAV, "MOTOR2") == 0, "could not start MOTOR2");
    sbo(PIN_CS1_MOTOR);
    u8 level = 0;
    for (int i = 0; i < 32; i++)
    {
        level ^= 1; if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        burn(CELL_CYCLES);
    }
    sbz(PIN_CS1_MOTOR);
    burn(TI99_CPU_HZ * 3);                  // three seconds with the motor stopped
    cassette_frame_tick();
    sbo(PIN_CS1_MOTOR);
    for (int i = 0; i < 32; i++)
    {
        level ^= 1; if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        burn(CELL_CYCLES);
    }
    burn(CELL_CYCLES * 4);
    sbz(PIN_CS1_MOTOR);
    cassette_eject();

    long withPause = ti99_file_size("/saves/ti99/tapes/MOTOR2.wav");
    long slack = withMotor / 8;
    CHECK(withPause < withMotor + slack,
          "a 3s motor-off pause added %ld bytes (%ld vs %ld) - the transport is running while stopped",
          withPause - withMotor, withPause, withMotor);
}

// =====================================================================================
// 6. The flows the console actually drives
//
// SAVE CS1 names no file, so the deck starts empty and the console's own CRU traffic has
// to be what asks for a tape. And BASIC's CHECK TAPE reads straight back after writing,
// with nothing but the DSR's on-screen prompts telling the user to rewind - so the deck
// has to make that turn by itself. Both of these failed on hardware first time round:
// with an empty deck every write went nowhere, the verify found no data, and the console
// reported I/O ERROR 66.
// =====================================================================================
static void write_pattern(const u8 *bits, int nbits)
{
    u8 level = 0;
    for (int i = 0; i < nbits; i++)
    {
        level ^= 1;
        if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        if (bits[i])
        {
            burn(HALF_CYCLES);
            level ^= 1;
            if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
            burn(CELL_CYCLES - HALF_CYCLES);
        }
        else burn(CELL_CYCLES);
    }
    burn(CELL_CYCLES * 4);
}

static void test_console_driven(void)
{
    printf("Console-driven save and verify\n");

    cassette_eject();
    CHECK(cassette_mode() == TAPE_EMPTY, "deck should start empty");
    CHECK(cassette_pending_request() == TAPE_REQ_NONE, "no request before the console asks");

    // SAVE CS1: the DSR starts the motor and writes the leader.
    sbo(PIN_CS1_MOTOR);
    sbz(PIN_TAPE_OUT);
    burn(CELL_CYCLES);
    sbo(PIN_TAPE_OUT);
    burn(CELL_CYCLES);

    CHECK(cassette_pending_request() == TAPE_REQ_RECORD,
          "writing with an empty deck must ask for a tape to record onto (got %d)",
          cassette_pending_request());

    // What the front end does with that: prompt for a label, then arm the deck.
    cassette_clear_request();
    CHECK(cassette_commit(-1, TAPE_RECORD_WAV, "CONSOLE") == 0, "could not arm recording");
    CHECK(cassette_mode() == TAPE_RECORD_WAV, "deck should be recording");

    u8 bits[160];
    int n = 0;
    for (int i = 0; i < 96; i++) bits[n++] = 0;              // leader
    static const u8 pat[] = {1,1,1,1,1,1,1,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0};
    for (size_t i = 0; i < sizeof(pat); i++) bits[n++] = pat[i];

    write_pattern(bits, n);

    // "PRESS CASSETTE STOP", then "REWIND ... THEN PRESS ENTER", then "PRESS PLAY".
    //
    // The console does NOT drop the motor across those prompts - it is telling the user to
    // stop the recorder by hand, and the relay stays closed until they have worked through
    // all three. A tape recorded on hardware shows it plainly: 5.6s of data followed by 69s
    // of the transport still rolling. So the only thing that marks the end of the save is
    // the data line going quiet, and the test has to model it that way; an earlier version
    // cycled the motor here and hid the bug completely.
    for (int i = 0; i < 240; i++) { burn(TI99_CPU_HZ / 60); cassette_frame_tick(); }

    // The first read is what tells the deck to turn round.
    u8 first = tb(PIN_TAPE_IN);
    (void)first;
    CHECK(cassette_mode() == TAPE_PLAY,
          "reading after a save must rewind and play back what was just written (mode %d)",
          (int)cassette_mode());
    CHECK(cassette_pending_request() == TAPE_REQ_NONE,
          "the verify pass must not prompt for a tape - it already knows which one");

    // And the data has to actually be there. This is the check that would have caught
    // "ERROR - NO DATA FOUND" before it reached hardware.
    u32 poll = HALF_CYCLES / 4;
    u8 prev = tb(PIN_TAPE_IN);
    u32 sinceEdge = 0, halfEst = HALF_CYCLES;
    u8 got[256];
    int gotn = 0, shortPending = 0, idle = 0;

    while (gotn < n + 4 && idle < 300)
    {
        burn(poll);
        cassette_frame_tick();
        sinceEdge += poll;
        u8 now = tb(PIN_TAPE_IN);
        if (now == prev) { idle++; continue; }
        prev = now; idle = 0;
        u32 gap = sinceEdge; sinceEdge = 0;
        if (gap < halfEst + halfEst / 2)
        {
            halfEst = (halfEst * 7 + gap) / 8;
            if (shortPending) { got[gotn++] = 1; shortPending = 0; }
            else shortPending = 1;
        }
        else { got[gotn++] = 0; shortPending = 0; }
    }
    sbz(PIN_CS1_MOTOR);

    CHECK(gotn > n / 2, "verify pass recovered only %d bits of %d - the tape read back empty", gotn, n);
    int off = find_alignment(bits, n, got, gotn);
    CHECK(off >= 0 && off <= 4, "verify pass data does not match what was written (offset %d)", off);

    cassette_eject();
}

// Now that clock-mode reads reach the tape, the DSR is free to glance at the input line
// in the middle of a save. Turning the deck round on that would truncate the recording
// at whatever point it happened.
static void test_read_during_save(void)
{
    printf("Read during a save\n");

    cassette_eject();
    CHECK(cassette_commit(-1, TAPE_RECORD_WAV, "MIDREAD") == 0, "could not arm recording");
    sbo(PIN_CS1_MOTOR);

    u8 level = 0;
    for (int i = 0; i < 64; i++)
    {
        level ^= 1;
        if (level) sbo(PIN_TAPE_OUT); else sbz(PIN_TAPE_OUT);
        burn(CELL_CYCLES);
        (void)tb(PIN_TAPE_IN);              // the DSR peeking at the input mid-save
        CHECK_ONCE(cassette_mode() == TAPE_RECORD_WAV,
                   "a read during a save turned the tape round and truncated it");
    }
    burn(CELL_CYCLES * 4);
    sbz(PIN_CS1_MOTOR);
    cassette_eject();

    long sz = ti99_file_size("/saves/ti99/tapes/MIDREAD.wav");
    CHECK(sz > 2000, "recording was cut short: only %ld bytes", sz);
}

// Decode the tape the DSR's way - poll the input and time the gaps - with the motor
// already running. Returns the number of bits recovered.
static int decode_running_tape(u8 *out, int maxbits)
{
    u32 poll = HALF_CYCLES / 4;
    u8 prev = tb(PIN_TAPE_IN);
    u32 sinceEdge = 0, halfEst = HALF_CYCLES;
    int n = 0, shortPending = 0, idle = 0;

    while (n < maxbits && idle < 300)
    {
        burn(poll);
        cassette_frame_tick();
        sinceEdge += poll;
        u8 now = tb(PIN_TAPE_IN);
        if (now == prev) { idle++; continue; }
        prev = now; idle = 0;
        u32 gap = sinceEdge; sinceEdge = 0;
        if (gap < halfEst + halfEst / 2)
        {
            halfEst = (halfEst * 7 + gap) / 8;
            if (shortPending) { out[n++] = 1; shortPending = 0; }
            else shortPending = 1;
        }
        else { out[n++] = 0; shortPending = 0; }
    }
    return n;
}

// SAVE, verify, then OLD in the same session. After the verify the deck is still loaded
// and sitting at the end of the tape, so a second load has to wind back - which is what
// the console has just told the user to do. Without it the read runs off the end and the
// console reports no data, exactly as if nothing had been recorded.
static void test_load_after_save(void)
{
    printf("OLD after SAVE in one session\n");

    cassette_eject();
    CHECK(cassette_commit(-1, TAPE_RECORD_WAV, "AGAIN") == 0, "could not arm recording");

    u8 bits[160];
    int n = 0;
    for (int i = 0; i < 96; i++) bits[n++] = 0;
    static const u8 pat[] = {1,1,1,1,1,1,1,1, 0,1,1,0,1,0,0,1, 1,0,0,1,0,1,1,0};
    for (size_t i = 0; i < sizeof(pat); i++) bits[n++] = pat[i];

    sbo(PIN_CS1_MOTOR);
    write_pattern(bits, n);
    for (int i = 0; i < 120; i++) { burn(TI99_CPU_HZ / 60); cassette_frame_tick(); }

    // The CHECK TAPE pass reads it straight back.
    u8 got[256];
    int gotn = decode_running_tape(got, n + 4);
    CHECK(cassette_mode() == TAPE_PLAY, "verify pass did not turn the tape round");
    CHECK(find_alignment(bits, n, got, gotn) >= 0, "verify pass did not read back cleanly");

    // Now OLD CS1: the user works through the rewind and play prompts, then the console
    // starts reading again. The deck is still loaded, and parked at the end.
    for (int i = 0; i < 300; i++) { burn(TI99_CPU_HZ / 60); cassette_frame_tick(); }

    gotn = decode_running_tape(got, n + 4);
    CHECK(gotn > n / 2, "second load recovered only %d bits of %d - the tape was not "
                        "wound back and read off the end", gotn, n);
    CHECK(find_alignment(bits, n, got, gotn) >= 0, "second load did not match what was written");

    sbz(PIN_CS1_MOTOR);
    cassette_eject();
}

// Loading a tape chosen from the settings menu, in a session that has already saved.
//
// The catch is that the console leaves the motor relay closed - it asks the *user* to
// press stop - so the transport is still running while the tape is picked and OLD CS1 is
// typed. By the time the DSR reads, the tape has rolled seconds past the data. Keying the
// rewind off "time since the last read" alone missed this, because mounting a tape clears
// the read history and the very first read is the one that matters here.
static void test_fresh_load_after_rolling(void)
{
    printf("Fresh load with the transport already rolling\n");

    // Put a tape on the card.
    u8 bits[160];
    int n = 0;
    for (int i = 0; i < 96; i++) bits[n++] = 0;
    static const u8 pat[] = {1,1,1,1,1,1,1,1, 1,0,1,1,0,0,1,0, 0,1,0,0,1,1,0,1};
    for (size_t i = 0; i < sizeof(pat); i++) bits[n++] = pat[i];
    record_bits(bits, n, TAPE_RECORD_WAV, "FRESH");

    // The console still has the motor on from the save it just did.
    sbo(PIN_CS1_MOTOR);

    // The user opens the settings menu and picks the tape.
    cassette_refresh_list();
    int idx = -1;
    for (int i = 0; i < cassette_num_tapes(); i++)
        if (strncmp(cassette_tape_name(i), "FRESH", 5) == 0) idx = i;
    CHECK(idx >= 0, "FRESH.wav was not listed");
    if (idx < 0) return;
    CHECK(cassette_commit(idx, TAPE_PLAY, NULL) == 0, "could not mount FRESH for playback");

    // Then types OLD CS1 and works through two prompts - eight seconds, with the
    // transport running the whole time.
    for (int i = 0; i < 480; i++) { burn(TI99_CPU_HZ / 60); cassette_frame_tick(); }

    u8 got[256];
    int gotn = decode_running_tape(got, n + 4);

    CHECK(gotn > n / 2, "fresh load recovered only %d bits of %d - the tape had rolled "
                        "past the data and was not wound back", gotn, n);
    CHECK(find_alignment(bits, n, got, gotn) >= 0, "fresh load did not match the tape");

    sbz(PIN_CS1_MOTOR);
    cassette_eject();
}

// Load the same tape repeatedly, and load one recorded by real hardware.
//
// TEST4.wav in the repo root came off a console: its timing is the DSR's, not this
// harness's, so decoding it is the first check of the detector against something our own
// encoder did not produce. Skipped if it is not there.
static void test_reload_same_tape(void)
{
    printf("Reloading a tape\n");

    if (system("cp -f ../../TEST4.wav sandbox/HW.wav 2>/dev/null") != 0)
    {
        printf("  (no TEST4.wav in the repo root - skipping the hardware tape)\n");
        return;
    }

    cassette_refresh_list();
    int idx = -1;
    for (int i = 0; i < cassette_num_tapes(); i++)
        if (strncmp(cassette_tape_name(i), "HW", 2) == 0) idx = i;
    CHECK(idx >= 0, "HW.wav was not listed");
    if (idx < 0) return;

    // Three loads in a row off the one tape, without ejecting or remounting.
    CHECK(cassette_commit(idx, TAPE_PLAY, NULL) == 0, "could not mount the hardware tape");

    int firstBits = 0;
    for (int pass = 1; pass <= 3; pass++)
    {
        sbo(PIN_CS1_MOTOR);

        // The whole tape is ~7300 bits: a 6144-bit leader, the marker, then the records.
        static u8 got[8192];
        int n = decode_running_tape(got, 8192);

        // A TI tape opens with a long run of zeros - the 768-byte leader - and then a
        // >FF marker. Finding both means the deck really did read the tape.
        int zeros = 0;
        while (zeros < n && got[zeros] == 0) zeros++;
        int ones = 0;
        for (int i = zeros; i < n && i < zeros + 8; i++) ones += got[i];

        printf("    pass %d: %d bits, %d leader zeros, %d ones after them\n",
               pass, n, zeros, ones);

        CHECK(zeros > 4000, "pass %d saw only %d leader bits - a 768-byte leader is 6144",
              pass, zeros);
        CHECK(ones == 8, "pass %d did not find the >FF marker after the leader", pass);

        if (pass == 1) firstBits = n;
        else CHECK(n > firstBits - 8 && n < firstBits + 8,
                   "pass %d recovered %d bits, first pass got %d - reloading is not "
                   "giving the same tape back", pass, n, firstBits);

        // Between loads the console sits at its prompts with the motor still running.
        sbz(PIN_CS1_MOTOR);
        for (int i = 0; i < 120; i++) { burn(TI99_CPU_HZ / 60); cassette_frame_tick(); }
    }

    cassette_eject();
}

// Read a dump of a physical cassette, if one has been put in place.
//
// This is the case the adaptive threshold exists for and the one nothing else here
// covers: a tape that has been through a real recorder, with its DC offset, level drift
// and wandering speed, in whatever format the person who dumped it happened to use -
// commonly 32-bit float at 48kHz, which is what a PC recording program writes.
//
//   TAPETEST_REAL=/path/to/dump.wav make -C tools/tapetest run
static void test_real_cassette(void)
{
    const char *src = getenv("TAPETEST_REAL");
    if (!src) return;

    printf("Real cassette dump\n");

    // Make runs this from tools/tapetest, so a path given relative to the repo root -
    // which is how anyone would naturally type it - needs the prefix adding.
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "cp -f '%s' sandbox/REALTAPE.wav 2>/dev/null || "
                               "cp -f '../../%s' sandbox/REALTAPE.wav", src, src);
    if (system(cmd) != 0)
    {
        printf("  FAIL could not read %s - check the path\n", src);
        failures++; checks++;
        return;
    }

    cassette_refresh_list();
    int idx = -1;
    for (int i = 0; i < cassette_num_tapes(); i++)
        if (strncmp(cassette_tape_name(i), "REALTAPE", 8) == 0) idx = i;
    CHECK(idx >= 0, "REALTAPE.wav was not listed");
    if (idx < 0) return;

    CHECK(cassette_commit(idx, TAPE_PLAY, NULL) == 0, "the deck would not open %s", src);
    if (cassette_mode() != TAPE_PLAY) return;

    sbo(PIN_CS1_MOTOR);

    static u8 got[16384];
    int n = decode_running_tape(got, 16384);

    int zeros = 0;
    while (zeros < n && got[zeros] == 0) zeros++;
    int ones = 0;
    for (int i = zeros; i < n && i < zeros + 8; i++) ones += got[i];

    printf("    %d bits, %d leader zeros, %d ones after them\n", n, zeros, ones);

    // A TI tape opens with 768 bytes of >00 and then a >FF marker. Allow a little slack
    // at the head for the detector settling, but the structure has to be there.
    CHECK(zeros > 5800 && zeros < 6300,
          "leader was %d bits, expected about 6144 - the detector is not tracking this tape",
          zeros);
    CHECK(ones == 8, "no >FF marker after the leader");

    sbz(PIN_CS1_MOTOR);
    cassette_eject();
}

// A load with an empty deck has to ask too, and must not ask twice in the same pass.
static void test_load_request(void)
{
    printf("Console-driven load\n");

    cassette_eject();
    sbo(PIN_CS1_MOTOR);
    burn(CELL_CYCLES);
    (void)tb(PIN_TAPE_IN);

    CHECK(cassette_pending_request() == TAPE_REQ_PLAY,
          "reading with an empty deck must ask for a tape to play (got %d)",
          cassette_pending_request());

    // The front end declines (the user cancelled the picker). Polling on must not queue
    // the prompt up again and again.
    cassette_clear_request();
    for (int i = 0; i < 50; i++) { burn(CELL_CYCLES); (void)tb(PIN_TAPE_IN); }
    CHECK(cassette_pending_request() == TAPE_REQ_NONE,
          "a declined prompt must not re-arm while the motor keeps running");

    // A fresh pass may ask again.
    sbz(PIN_CS1_MOTOR);
    sbo(PIN_CS1_MOTOR);
    (void)tb(PIN_TAPE_IN);
    CHECK(cassette_pending_request() == TAPE_REQ_PLAY,
          "a new motor-on pass should ask again");

    cassette_clear_request();
    sbz(PIN_CS1_MOTOR);
    cassette_eject();
}

// =====================================================================================
// 5. WAV files this has to be able to read
// =====================================================================================
static void put32le(FILE *f, u32 v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); fputc((v >> 16) & 0xFF, f); fputc((v >> 24) & 0xFF, f); }
static void put16le(FILE *f, u16 v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }

// Write a bi-phase-mark tape as a WAV in an arbitrary format, the way a real dump or a
// PC-side converter might present it.
static void write_test_wav(const char *path, const u8 *bits, int nbits,
                           u32 rate, int bitsPer, int channels, int dcOffset, int amplitude)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  FAIL cannot write %s\n", path); failures++; return; }

    int frameBytes = channels * (bitsPer / 8);
    // Header first with placeholder sizes.
    fwrite("RIFF", 1, 4, f); put32le(f, 0);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); put32le(f, 16);
    put16le(f, 1); put16le(f, channels); put32le(f, rate);
    put32le(f, rate * frameBytes); put16le(f, frameBytes); put16le(f, bitsPer);
    fwrite("data", 1, 4, f); put32le(f, 0);

    u32 frames = 0;
    double cell = (double)rate / TAPE_BAUD;
    double pos = 0, nextEdge = 0;
    int level = 0, bitIdx = 0, midCell = 0;
    double total = cell * nbits;

    while (pos < total)
    {
        while (pos >= nextEdge)
        {
            level ^= 1;
            if (midCell) { nextEdge += cell / 2; midCell = 0; }
            else
            {
                u8 b = (bitIdx < nbits) ? bits[bitIdx++] : 0;
                if (b) { nextEdge += cell / 2; midCell = 1; }
                else     nextEdge += cell;
            }
        }
        int v = level ? amplitude : -amplitude;
        v += dcOffset;
        for (int c = 0; c < channels; c++)
        {
            if (bitsPer == 8) fputc((v >> 8) + 128, f);
            else              put16le(f, (u16)(s16)v);
        }
        frames++;
        pos += 1.0;
    }

    u32 dataBytes = frames * frameBytes;
    fseek(f, 4, SEEK_SET);  put32le(f, 36 + dataBytes);
    fseek(f, 40, SEEK_SET); put32le(f, dataBytes);
    fclose(f);
}

static void test_wav_formats(void)
{
    printf("WAV formats\n");

    u8 bits[128];
    int n = 0;
    for (int i = 0; i < 48; i++) bits[n++] = 0;
    static const u8 pattern[] = {1,1,1,1,1,1,1,1, 1,0,1,1,0,0,1,0, 0,1,0,0,1,1,0,1};
    for (size_t i = 0; i < sizeof(pattern); i++) bits[n++] = pattern[i];

    struct { const char *name; u32 rate; int bits; int ch; int dc; int amp; } variants[] = {
        { "V8MONO",   22050, 8,  1, 0,      12000 },   // 8-bit mono, the classic dump
        { "V16MONO",  44100, 16, 1, 0,      20000 },
        { "V16ST",    44100, 16, 2, 0,      20000 },   // stereo: left channel only
        { "VDCOFF",   22050, 16, 1, 6000,   9000  },   // heavy DC offset
        { "VQUIET",   22050, 16, 1, -1500,  1800  },   // recorded far too quietly
        { "V8K",      8000,  8,  1, 0,      12000 },   // barely enough samples per cell
    };

    for (size_t v = 0; v < sizeof(variants) / sizeof(variants[0]); v++)
    {
        char path[256];
        snprintf(path, sizeof(path), "sandbox/%s.wav", variants[v].name);
        write_test_wav(path, bits, n, variants[v].rate, variants[v].bits,
                       variants[v].ch, variants[v].dc, variants[v].amp);

        u8 got[256];
        int gotn = playback_bits(variants[v].name, got, n + 8);

        int off = find_alignment(bits, n, got, gotn);
        CHECK(off >= 0 && off <= 8,
              "%s (%u Hz %d-bit %dch dc=%d amp=%d): %d bits recovered, none of them align",
              variants[v].name, variants[v].rate, variants[v].bits, variants[v].ch,
              variants[v].dc, variants[v].amp, gotn);
    }
}

// =====================================================================================
int main(void)
{
    if (system("rm -rf sandbox && mkdir -p sandbox")) return 2;

    memset(&tms9900, 0, sizeof(tms9900));
    TMS9901_Reset();
    cassette_init();

    test_timer();
    test_cru_decode();
    test_write_from_clock_mode();
    test_read_from_clock_mode();

    TMS9901_Reset();
    cassette_init();
    test_roundtrip(TAPE_RECORD_WAV, "WAV");
    test_roundtrip(TAPE_RECORD_CAS, "CAS");
    test_cas_calibration();
    test_motor_gate();
    test_wav_formats();
    test_console_driven();
    test_read_during_save();
    test_load_after_save();
    test_fresh_load_after_rolling();
    test_reload_same_tape();
    test_real_cassette();
    test_load_request();

    cassette_shutdown();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
