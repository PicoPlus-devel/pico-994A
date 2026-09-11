// =====================================================================================
// speech.c - TMS5200 / TMS5220 LPC speech synthesiser. See speech.h.
//
// The synthesis model is the one described in TI's own patents - US 4,209,804 (the
// lattice filter, table I), US 4,331,836 (the chirp excitation) and US 4,335,277 (the
// frame/command logic) - which is also what every emulator of these chips implements.
// The coefficient tables are the chip's own data, published in those patents and
// verified against die decaps by the MAME project.
//
// This is a functional model, not a cycle-accurate one: it runs a frame as eight
// interpolation periods of 25 samples rather than reproducing the chip's internal
// PC/subcycle counters. That is inaudible for playback and much cheaper.
// =====================================================================================
#include <string.h>
#ifdef TI99_SPEECH_TRACE
#include <stdio.h>
#endif
#include "ti99_compat.h"
#include "ti99.h"
#include "speech.h"

// =====================================================================================
// Coefficient tables
// =====================================================================================

// Energy is common to both chips (the "later 028X" table).
static const u16 energytable[16] =
{
      0,   1,   2,   3,   4,   6,   8,  11,
     16,  23,  33,  47,  63,  85, 114,   0
};

// Pitch differs between the two chips.
static const u16 pitchtable_5200[64] =
{
      0,  14,  15,  16,  17,  18,  19,  20,
     21,  22,  23,  24,  25,  26,  27,  28,
     29,  30,  31,  32,  34,  36,  38,  40,
     41,  43,  45,  48,  49,  51,  54,  55,
     57,  60,  62,  64,  68,  72,  74,  76,
     81,  85,  87,  90,  96,  99, 103, 107,
    112, 117, 122, 127, 133, 139, 145, 151,
    157, 164, 171, 178, 186, 194, 202, 211
};

static const u16 pitchtable_5220[64] =
{
      0,  15,  16,  17,  18,  19,  20,  21,
     22,  23,  24,  25,  26,  27,  28,  29,
     30,  31,  32,  33,  34,  35,  36,  37,
     38,  39,  40,  41,  42,  44,  46,  48,
     50,  52,  53,  56,  58,  60,  62,  65,
     68,  70,  72,  76,  78,  80,  84,  86,
     91,  94,  98, 101, 105, 109, 114, 118,
    122, 127, 132, 137, 142, 148, 153, 159
};

// Reflection coefficients. K1 and K2 are 5-bit indices, K3..K7 4-bit, K8..K10 3-bit.
static const s16 k1_5200[32] = {
    -501, -498, -495, -490, -485, -478, -469, -459,
    -446, -431, -412, -389, -362, -331, -295, -253,
    -207, -156, -102,  -45,   13,   70,  126,  179,
     228,  272,  311,  345,  374,  399,  420,  437 };
static const s16 k2_5200[32] = {
    -376, -357, -335, -312, -286, -258, -227, -195,
    -161, -124,  -87,  -49,  -10,   29,   68,  106,
     143,  178,  212,  243,  272,  299,  324,  346,
     366,  384,  400,  414,  427,  438,  448,  506 };
static const s16 k3_5200[16] = {
    -407, -381, -349, -311, -268, -218, -162, -102,
     -39,   25,   89,  149,  206,  257,  302,  341 };
static const s16 k4_5200[16] = {
    -290, -252, -209, -163, -114,  -62,   -9,   44,
      97,  147,  194,  238,  278,  313,  344,  371 };
static const s16 k5_5200[16] = {
    -318, -283, -245, -202, -156, -107,  -56,   -3,
      49,  101,  150,  196,  239,  278,  313,  344 };
static const s16 k6_5200[16] = {
    -193, -152, -109,  -65,  -20,   26,   71,  115,
     158,  198,  235,  270,  301,  330,  355,  377 };
static const s16 k7_5200[16] = {
    -254, -218, -180, -140,  -97,  -53,   -8,   36,
      81,  124,  165,  204,  240,  274,  304,  332 };
// K9's 4th entry is 0x3E0 (-32). Patents 4,403,965 and 4,946,391 print 0x3ED (-19),
// which is a typo; the decapped part gives -32.
static const s16 k8_5200[8]  = { -205, -112,  -10,   92,  187,  269,  336,  387 };
static const s16 k9_5200[8]  = { -249, -183, -110,  -32,   48,  126,  198,  261 };
static const s16 k10_5200[8] = { -190, -133,  -73,  -10,   53,  115,  173,  227 };

static const s16 k1_5220[32] = {
    -501, -498, -497, -495, -493, -491, -488, -482,
    -478, -474, -469, -464, -459, -452, -445, -437,
    -412, -380, -339, -288, -227, -158,  -81,   -1,
      80,  157,  226,  287,  337,  379,  411,  436 };
static const s16 k2_5220[32] = {
    -328, -303, -274, -244, -211, -175, -138,  -99,
     -59,  -18,   24,   64,  105,  143,  180,  215,
     248,  278,  306,  331,  354,  374,  392,  408,
     422,  435,  445,  455,  463,  470,  476,  506 };
static const s16 k3_5220[16] = {
    -441, -387, -333, -279, -225, -171, -117,  -63,
      -9,   45,   98,  152,  206,  260,  314,  368 };
static const s16 k4_5220[16] = {
    -328, -273, -217, -161, -106,  -50,    5,   61,
     116,  172,  228,  283,  339,  394,  450,  506 };
static const s16 k5_5220[16] = {
    -328, -282, -235, -189, -142,  -96,  -50,   -3,
      43,   90,  136,  182,  229,  275,  322,  368 };
static const s16 k6_5220[16] = {
    -256, -212, -168, -123,  -79,  -35,   10,   54,
      98,  143,  187,  232,  276,  320,  365,  409 };
static const s16 k7_5220[16] = {
    -308, -260, -212, -164, -117,  -69,  -21,   27,
      75,  122,  170,  218,  266,  314,  361,  409 };
static const s16 k8_5220[8]  = { -256, -161,  -66,   29,  124,  219,  314,  409 };
static const s16 k9_5220[8]  = { -256, -176,  -96,  -15,   65,  146,  226,  307 };
static const s16 k10_5220[8] = { -205, -132,  -59,   14,   87,  160,  234,  307 };

// Voiced excitation. The chirp is followed by silence: past entry 51 the chip stops
// incrementing the index, so every later sample of a pitch period repeats entry 51.
static const s8 chirptable[52] =
{
    0x00, 0x03, 0x0f, 0x28, 0x4c, 0x6c, 0x71, 0x50,
    0x25, 0x26, 0x4c, 0x44, 0x1a, 0x32, 0x3b, 0x13,
    0x37, 0x1a, 0x25, 0x1f, 0x1d, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

// Bits per K index, K1..K10.
static const u8 kbits[10] = { 5, 5, 4, 4, 4, 4, 4, 3, 3, 3 };

// Interpolation shift per period. Applied in the order the chip uses them: periods 1..7
// each close a fraction of the gap to the new frame's values, then period 0 snaps
// exactly onto them just before the next frame is parsed.
static const u8 interp_shift[8] = { 0, 3, 3, 3, 2, 2, 1, 1 };

static const s16 *ktables_5200[10] = {
    k1_5200, k2_5200, k3_5200, k4_5200, k5_5200,
    k6_5200, k7_5200, k8_5200, k9_5200, k10_5200 };
static const s16 *ktables_5220[10] = {
    k1_5220, k2_5220, k3_5220, k4_5220, k5_5220,
    k6_5220, k7_5220, k8_5220, k9_5220, k10_5220 };

// Active table set - swapped by SpeechSetChip().
static const s16 **ktable   = ktables_5200;
static const u16  *pitchtab = pitchtable_5200;

// =====================================================================================
// Chip state
// =====================================================================================
#define FIFO_SIZE   16          // TMS5220 FIFO is 16 bytes

// Status bits returned on a read (D0 is the MSB on this bus).
#define STAT_TS     0x80        // Talk Status - the chip is speaking
#define STAT_BL     0x40        // Buffer Low - FIFO half empty or less
#define STAT_BE     0x20        // Buffer Empty

typedef struct
{
    u8   present;               // a Speech Synthesizer is attached at all

    // --- command interface ---
    u8   fifo[FIFO_SIZE];
    u8   fifo_head, fifo_tail, fifo_count;
    u8   fifo_bit;              // bits already consumed from the head byte
    u8   status;
    u8   read_pending;          // a Read Byte command is waiting to be collected
    u8   read_data;

    // --- TMS6100 vocabulary ROM ---
    u8  *rom;
    u32  rom_size;
    u32  rom_addr;
    u8   addr_nibbles;          // how many of the five Load Address nibbles have arrived

    // --- synthesis ---
    u8   talking;               // producing samples
    u8   speak_external;        // DDIS: frame bits come from the FIFO rather than the ROM
    u8   buffer_low;            // FIFO at or below half - tracked for its falling edge
    u8   stopping;              // a stop frame was seen; finish the current frame then halt

    u8   new_energy_idx, new_pitch_idx, new_k_idx[10];
    u8   old_unvoiced, old_silence;
    u8   inhibit;               // suppress interpolation across a voicing/silence change
    u8   zpar, uv_zpar;         // zero all params / zero K5..K10

    int  cur_energy, cur_pitch, cur_k[10];
    int  x[11], u[11];          // lattice filter state
    u16  rng;
    int  pitch_count;

    u8   ip;                    // interpolation period, 0..7 within the frame
    u8   sample_in_period;      // 0..24
    u8   frame_started;
} tms52xx_t;

static tms52xx_t sp;

// =====================================================================================
// Trace counters (-DTI99_SPEECH_TRACE)
//
// Everything the CPU side does to the chip, tallied. The point is to separate causes
// that all sound the same: never addressed, addressed but never told to speak, told to
// speak but never enough bytes to start, started but starved, or started and finished
// normally with the fault further down in the synthesiser or the mixer.
// =====================================================================================
#ifdef TI99_SPEECH_TRACE
typedef struct
{
    u32 writes, reads;
    u32 data_bytes;                 // writes that went into the FIFO
    u32 cmd_readbyte, cmd_readbranch, cmd_loadaddr, cmd_speak, cmd_speakext, cmd_reset;
    u32 talk_starts, halts;
    u32 stop_frames, underruns;     // clean end of phrase vs FIFO ran dry mid-frame
    u32 rom_reads;                  // Read Byte answered out of the vocabulary ROM
    u32 fifo_hw;                    // FIFO high-water mark
    u32 samples;                    // times SpeechGetSample() was asked for output
    u8  last_status, last_rom_byte;
    u32 last_rom_addr;
} speech_trace_t;

static speech_trace_t trc;
static u32 trc_dirty  = 0;          // something happened since the last summary
static u32 trc_frames = 0;

#define TRC(field)  do { trc.field++; trc_dirty = 1; } while (0)
#else
#define TRC(field)  do { } while (0)
#endif

// =====================================================================================
// FIFO and bit extraction
// =====================================================================================
static void fifo_reset(void)
{
    sp.fifo_head = sp.fifo_tail = sp.fifo_count = sp.fifo_bit = 0;
}

static void fifo_push(u8 data)
{
    if (sp.fifo_count >= FIFO_SIZE) return;     // the real chip drops the byte too
    sp.fifo[sp.fifo_tail] = data;
    sp.fifo_tail = (sp.fifo_tail + 1) % FIFO_SIZE;
    sp.fifo_count++;
}

static void update_status(void)
{
    sp.status = 0;
    if (sp.talking)               sp.status |= STAT_TS;
    if (sp.speak_external)
    {
        if (sp.fifo_count <= 8)   sp.status |= STAT_BL;
        if (sp.fifo_count == 0)   sp.status |= STAT_BE;
    }
    else
    {
        // Not streaming: the buffer is meaningless, report it empty as the chip does.
        sp.status |= (STAT_BL | STAT_BE);
    }
}

// Pull one bit from whichever source is feeding the frame decoder.
//
// Bit order is the thing to get right here, and the two sources differ:
//
// - Speak External (FIFO) data runs **least significant bit first within each byte**,
//   the order cartridges store it in. Fields are still assembled most significant bit
//   first out of that stream, so a byte is effectively read back to front. (This is why
//   the Talkie library bit-reverses every byte before parsing it.) Proven against all 22
//   of Parsec's phrases: each consumes exactly its declared length and ends on a stop
//   frame.
// - The vocabulary ROM image (spchrom.bin) is stored the other way round, **most
//   significant bit first**. Its word tree gives every word's start address and length,
//   and read MSB-first all 373 words end on a stop frame within their declared length;
//   read LSB-first, HELLO stops after four frames. Read Byte is unaffected - the tree's
//   ASCII words and pointers are used byte for byte as stored.
//
// Getting either backwards still decodes *something* - the fields simply land on the
// wrong bits, the stream drifts, and sooner or later a 4-bit energy field reads as 15 and
// the phrase stops dead partway through.
//
// Returns 0 and sets `ran_out` when a Speak External stream is exhausted - the chip
// treats that exactly like a stop frame.
static u8 ran_out;

static u8 read_bit(void)
{
    u8 bit;

    if (sp.speak_external)
    {
        if (sp.fifo_count == 0) { ran_out = 1; return 0; }

        bit = (sp.fifo[sp.fifo_head] >> sp.fifo_bit) & 1;
        if (++sp.fifo_bit >= 8)
        {
            sp.fifo_bit = 0;
            sp.fifo_head = (sp.fifo_head + 1) % FIFO_SIZE;
            sp.fifo_count--;
        }
    }
    else
    {
        if (!sp.rom || sp.rom_size == 0) { ran_out = 1; return 0; }

        u32 addr = sp.rom_addr % sp.rom_size;
        bit = (sp.rom[addr] >> (7 - sp.fifo_bit)) & 1;
        if (++sp.fifo_bit >= 8)
        {
            sp.fifo_bit = 0;
            sp.rom_addr++;
        }
    }
    return bit;
}

// Fields are assembled most significant bit first out of the LSB-first bit stream.
static int read_bits(u8 count)
{
    int value = 0;
    while (count--) value = (value << 1) | read_bit();
    return value;
}

// =====================================================================================
// Frame decoding
//
// Frame layout, MSB first:
//   energy   4 bits   0 = silence frame, 15 = stop frame
//   repeat   1 bit    reuse the previous frame's coefficients
//   pitch    6 bits   0 = unvoiced
//   K1..K4   5,5,4,4  present unless this is a repeat frame
//   K5..K10  4,4,4,3,3,3  present only when the frame is voiced
// =====================================================================================
static void parse_frame(void)
{
    ran_out = 0;
    sp.zpar = sp.uv_zpar = 0;

    sp.new_energy_idx = (u8)read_bits(4);
    if (ran_out) { sp.stopping = 1; TRC(underruns); return; }

    if (sp.new_energy_idx == 0)
    {
        // Silence frame: every parameter goes to zero, but the chip keeps running.
        sp.zpar = 1;
        return;
    }
    if (sp.new_energy_idx == 15)
    {
        // Stop frame: finish out this frame's energy ramp, then halt.
        sp.stopping = 1;
        TRC(stop_frames);
        return;
    }

    u8 repeat = (u8)read_bits(1);
    if (ran_out) { sp.stopping = 1; TRC(underruns); return; }

    sp.new_pitch_idx = (u8)read_bits(6);
    if (ran_out) { sp.stopping = 1; TRC(underruns); return; }

    // An unvoiced frame carries no K5..K10; they are forced to zero instead.
    sp.uv_zpar = (sp.new_pitch_idx == 0) ? 1 : 0;

    if (repeat) return;         // reuse the coefficients already loaded

    for (int i = 0; i < 4; i++)
    {
        sp.new_k_idx[i] = (u8)read_bits(kbits[i]);
        if (ran_out) { sp.stopping = 1; TRC(underruns); return; }
    }

    if (sp.new_pitch_idx == 0) return;   // unvoiced: only four K's are transmitted

    for (int i = 4; i < 10; i++)
    {
        sp.new_k_idx[i] = (u8)read_bits(kbits[i]);
        if (ran_out) { sp.stopping = 1; TRC(underruns); return; }
    }
}

// Load the new frame's parameters as interpolation targets, deciding first whether
// interpolation should happen at all. The chip inhibits it across a change of voicing
// or in or out of silence, because interpolating between those is meaningless.
static void start_frame(void)
{
    u8 new_silence  = (sp.new_energy_idx == 0);
    u8 new_unvoiced = (sp.new_pitch_idx == 0);

    sp.inhibit = (sp.old_unvoiced != new_unvoiced) || (sp.old_silence != new_silence);

    sp.old_unvoiced = new_unvoiced;
    sp.old_silence  = new_silence;

    sp.ip = 0;
    sp.sample_in_period = 0;
}

// One interpolation step, run at the start of each of the eight periods in a frame.
//
// Period 0 is where the new frame's parameters arrive, and the chip does not interpolate
// there. Periods 1-7 then glide the current values toward the target by 1/8, 1/8, 1/8,
// 1/4, 1/4, 1/2, 1/2 - which deliberately falls about 9% short, because the following
// frame arrives and becomes the new target before the glide ever completes. That endless
// chase is the point: the filter's coefficients are always moving, never stepping.
//
// When interpolation is inhibited - across a change of voicing, or in or out of silence -
// gliding between the two is meaningless, so the parameters take their new values at
// once, at period 0, and hold.
static void interpolate(void)
{
    int target_energy = sp.zpar ? 0 : energytable[sp.new_energy_idx];
    int target_pitch  = sp.zpar ? 0 : pitchtab[sp.new_pitch_idx];

    if (sp.ip == 0)
    {
        if (!sp.inhibit) return;        // targets loaded; the glide starts next period

        sp.cur_energy = target_energy;
        sp.cur_pitch  = target_pitch;
        for (int i = 0; i < 10; i++)
            sp.cur_k[i] = (sp.zpar || (i >= 4 && sp.uv_zpar))
                        ? 0 : ktable[i][sp.new_k_idx[i]];
        return;
    }

    if (sp.inhibit) return;             // already taken, at period 0

    u8 shift = interp_shift[sp.ip];
    sp.cur_energy += (target_energy - sp.cur_energy) >> shift;
    sp.cur_pitch  += (target_pitch  - sp.cur_pitch)  >> shift;
    for (int i = 0; i < 10; i++)
    {
        int t = (sp.zpar || (i >= 4 && sp.uv_zpar)) ? 0 : ktable[i][sp.new_k_idx[i]];
        sp.cur_k[i] += (t - sp.cur_k[i]) >> shift;
    }
}

// =====================================================================================
// Synthesis
// =====================================================================================

// The chip's multiplier: 10-bit signed by 14-bit signed, result shifted down 9. The
// wrapping is the hardware's own behaviour on overflow, and some sounds depend on it.
static inline int matrix_multiply(int a, int b)
{
    while (a >  511)   a -= 1024;
    while (a < -512)   a += 1024;
    while (b >  16383) b -= 32768;
    while (b < -16384) b += 32768;
    return (a * b) >> 9;
}

// Ten-stage lattice filter, US patent 4,209,804 table I. u[] is the forward path down
// the top of the lattice, x[] the delayed reflections along the bottom.
static int lattice_filter(int excitation)
{
    sp.u[10] = matrix_multiply(sp.cur_energy, excitation << 6);

    for (int i = 9; i >= 0; i--)
        sp.u[i] = sp.u[i + 1] - matrix_multiply(sp.cur_k[i], sp.x[i]);

    for (int i = 9; i >= 1; i--)
        sp.x[i] = sp.x[i - 1] + matrix_multiply(sp.cur_k[i - 1], sp.u[i - 1]);

    sp.x[0] = sp.u[0];
    return sp.u[0];
}

// 13-bit LFSR, clocked twenty times per sample as the chip does, giving the unvoiced
// excitation its characteristic spectrum.
static inline void clock_rng(void)
{
    for (int i = 0; i < 20; i++)
    {
        u8 bit = ((sp.rng >> 12) & 1) ^ ((sp.rng >> 3) & 1) ^
                 ((sp.rng >>  2) & 1) ^ ((sp.rng >> 0) & 1);
        sp.rng = (u16)((sp.rng << 1) | bit);
    }
}

static void halt_speech(void);

s16 SpeechGetSample(void)
{
    if (!sp.talking) return 0;
    TRC(samples);

    // Start of an interpolation period: 25 samples each, eight to a 25ms frame.
    if (sp.sample_in_period == 0)
    {
        if (sp.ip == 0 && !sp.frame_started)
        {
            // Beginning of a frame - pull the next one out of the FIFO or the ROM.
            if (sp.stopping)
            {
                halt_speech();
                return 0;
            }
            parse_frame();
            start_frame();
            sp.frame_started = 1;
        }
        interpolate();
    }

    // Excitation: a chirp for voiced frames, noise for unvoiced.
    int excitation;
    if (sp.old_unvoiced || sp.cur_pitch == 0)
    {
        // Half of the chirp table's peak, positive or negative - per the patent.
        excitation = (sp.rng & 1) ? -64 : 64;
    }
    else
    {
        excitation = (sp.pitch_count >= 51) ? chirptable[51] : chirptable[sp.pitch_count];
    }

    clock_rng();
    int sample = lattice_filter(excitation);

    // Advance the pitch period.
    if (sp.cur_pitch > 0)
    {
        if (++sp.pitch_count >= sp.cur_pitch) sp.pitch_count = 0;
    }
    else
    {
        sp.pitch_count = 0;
    }

    // Advance the period / frame counters.
    if (++sp.sample_in_period >= 25)
    {
        sp.sample_in_period = 0;
        if (++sp.ip >= 8)
        {
            sp.ip = 0;
            sp.frame_started = 0;      // next call parses the following frame
        }
    }

    // Level. The lattice runs at the chip's own 14-bit scale, so the clip below is the
    // chip's, not ours - and real speech never reaches it: measured across all 22 of
    // Parsec's phrases the peak is ~5400 and the RMS ~400. Treating 16384 as full scale
    // and halving was therefore wrong twice over, and left a loud phrase peaking at 2682
    // - about 12 dB under a single PSG voice, which is why speech was almost inaudible
    // next to the game. Doubling instead puts a loud phrase at ~10700, right where one
    // PSG channel at full volume sits, and still leaves the mixer half its range for the
    // game's own sound. This is a pure gain change: the waveform is untouched.
    if (sample >  8191) sample =  8191;
    if (sample < -8192) sample = -8192;
    return (s16)(sample << 1);
}

u8 SpeechIsTalking(void) { return sp.talking; }

// =====================================================================================
// CPU-facing command interface
//
// The command is in the top nibble (bit 7 is don't-care):
//   x001  Read Byte          x011  Read and Branch
//   x100  Load Address       x101  Speak
//   x110  Speak External     x111  Reset
// Anything else is a data byte for the Speak External FIFO.
// =====================================================================================
static void halt_speech(void)
{
    TRC(halts);
    sp.talking        = 0;
    sp.speak_external = 0;      // the chip drops DDIS when talk status falls
    sp.stopping       = 0;
    sp.buffer_low     = 1;
    update_status();
}

// Put the synthesiser at rest so the first real frame interpolates up from silence
// rather than stepping the filter. The chip does the same by pre-loading a zero-energy,
// zero-pitch frame with the K indices at their neutral values.
static void prime_synth(void)
{
    sp.frame_started    = 0;
    sp.ip               = 0;
    sp.sample_in_period = 0;
    sp.fifo_bit         = 0;
    sp.stopping         = 0;

    sp.cur_energy = 0;
    sp.cur_pitch  = 0;
    memset(sp.cur_k, 0, sizeof(sp.cur_k));
    memset(sp.x, 0, sizeof(sp.x));
    memset(sp.u, 0, sizeof(sp.u));
    sp.pitch_count  = 0;

    sp.new_energy_idx = 0;
    sp.new_pitch_idx  = 0;
    memset(sp.new_k_idx, 0, sizeof(sp.new_k_idx));

    sp.old_unvoiced = 1;        // treat what came before as silence
    sp.old_silence  = 1;
    sp.zpar = sp.uv_zpar = 1;
}

void SpeechDataWrite(u8 data)
{
    if (!sp.present) return;
    TRC(writes);

    // ---------------------------------------------------------------------------------
    // While Speak External is active every write is a data byte for the FIFO - the chip
    // does no command decoding at all in that state. This matters: LPC data routinely
    // contains bytes that look exactly like commands.
    // ---------------------------------------------------------------------------------
    if (sp.speak_external)
    {
        fifo_push(data);
        TRC(data_bytes);
#ifdef TI99_SPEECH_TRACE
        if (sp.fifo_count > trc.fifo_hw) trc.fifo_hw = sp.fifo_count;
#endif

        // Speak External does not start the chip talking; it starts once enough bytes
        // have arrived to clear the buffer-low flag. Games rely on this to stream.
        u8 was_low = sp.buffer_low;
        sp.buffer_low = (sp.fifo_count <= 8);
        if (was_low && !sp.buffer_low && !sp.talking)
        {
            prime_synth();
            sp.talking = 1;
            TRC(talk_starts);
        }
        update_status();
        return;
    }

    // A Load Address sequence is five nibbles long and takes priority over command
    // decoding while it is in progress.
    if (sp.addr_nibbles > 0)
    {
        if ((data & 0x70) == 0x40)
        {
            // Nibbles arrive least-significant first, building a 20-bit ROM address.
            sp.rom_addr = (sp.rom_addr >> 4) | ((u32)(data & 0x0F) << 16);
            if (++sp.addr_nibbles > 5)
            {
                sp.addr_nibbles = 0;
                sp.rom_addr &= 0x000FFFFF;
                sp.fifo_bit = 0;                // start reading on a byte boundary
            }
            return;
        }
        sp.addr_nibbles = 0;                    // sequence abandoned
    }

    switch (data & 0x70)
    {
        case 0x70:      // Reset
            TRC(cmd_reset);
            SpeechInit();
            break;

        case 0x10:      // Read Byte from the vocabulary ROM
            TRC(cmd_readbyte);
            if (sp.talking) break;              // ignored while speaking
            if (sp.rom && sp.rom_size)
            {
#ifdef TI99_SPEECH_TRACE
                trc.rom_reads++;
                trc.last_rom_addr = sp.rom_addr % sp.rom_size;
#endif
                sp.read_data = sp.rom[sp.rom_addr % sp.rom_size];
                sp.rom_addr++;
            }
            else
            {
                sp.read_data = 0x00;
            }
            sp.read_pending = 1;
            break;

        case 0x30:      // Read and Branch - follow the two-byte pointer at the address
            TRC(cmd_readbranch);
            if (sp.talking) break;
            if (sp.rom && sp.rom_size >= 2)
            {
                u32 a  = sp.rom_addr % sp.rom_size;
                u32 hi = sp.rom[a];
                u32 lo = sp.rom[(a + 1) % sp.rom_size];
                sp.rom_addr = ((hi << 8) | lo) & 0x000FFFFF;
            }
            sp.fifo_bit = 0;
            break;

        case 0x40:      // Load Address - first of five nibbles
            TRC(cmd_loadaddr);
            if (sp.talking) break;
            sp.rom_addr = (sp.rom_addr >> 4) | ((u32)(data & 0x0F) << 16);
            sp.addr_nibbles = 2;                // this one has arrived, four to go
            break;

        case 0x50:      // Speak - frames come from the vocabulary ROM, starting now
            TRC(cmd_speak);
            TRC(talk_starts);
            prime_synth();
            sp.speak_external = 0;
            sp.talking = 1;
            update_status();
            break;

        case 0x60:      // Speak External - arm the FIFO and wait for it to fill
            TRC(cmd_speakext);
            fifo_reset();
            sp.speak_external = 1;
            sp.buffer_low     = 1;
            sp.talking        = 0;
            update_status();
            break;

        default:        // 0x00 / 0x20: NOP on this chip
            break;
    }
}

u8 SpeechDataRead(void)
{
    // With no module attached the address space floats high, which is how software
    // detects that speech is unavailable.
    if (!sp.present) return 0xFF;

    TRC(reads);

    if (sp.read_pending)
    {
        sp.read_pending = 0;
#ifdef TI99_SPEECH_TRACE
        trc.last_rom_byte = sp.read_data;
#endif
        return sp.read_data;
    }

    update_status();
#ifdef TI99_SPEECH_TRACE
    trc.last_status = sp.status;
#endif
    return sp.status;
}

void SpeechInit(void)
{
    u8  *rom  = sp.rom;             // survives a reset - it is the module's own ROM
    u32  size = sp.rom_size;
    u8   present = sp.present;

    memset(&sp, 0, sizeof(sp));

    sp.rom      = rom;
    sp.rom_size = size;
    sp.present  = present;
    sp.rng      = 0x1FFF;           // any non-zero seed; the chip powers up filled
    sp.old_unvoiced = 1;
    sp.old_silence  = 1;
    sp.buffer_low   = 1;
    fifo_reset();
    update_status();
}

void SpeechSetROM(u8 *rom, u32 size)
{
    sp.rom      = rom;
    sp.rom_size = size;
    sp.present  = 1;                // the module answers whether or not it has a ROM
}

void SpeechSetChip(u8 chipType)
{
    if (chipType == SPEECH_CHIP_TMS5220)
    {
        ktable   = ktables_5220;
        pitchtab = pitchtable_5220;
    }
    else
    {
        ktable   = ktables_5200;
        pitchtab = pitchtable_5200;
    }
}

u8 SpeechIsPresent(void) { return sp.present; }

// =====================================================================================
// Trace summary
//
// Called once per emulated frame. Prints at most once a second, and only when the CPU
// has touched the chip since the last line, so an idle machine stays quiet.
//
// Reading the line:
//   wr/rd 0/0          nothing ever addressed >9000-97FF - the fault is in the memory
//                      map or the game never even looked for the module
//   rd > 0, wr 0       the game probed and gave up: detection failed
//   ext 0              no Speak External - the game decided speech is absent
//   ext > 0, talk 0    the stream was armed but the FIFO never passed 8 bytes, so
//                      synthesis never started (watch hw - the high-water mark)
//   talk > 0, smp 0    talking, but nothing ever asked for samples - mixer side
//   under >> stop      the FIFO is being starved: the game is feeding it more slowly
//                      than we drain it, or we only drain once per frame in a lump
// =====================================================================================
#ifdef TI99_SPEECH_TRACE
void SpeechTraceTick(void)
{
    if (++trc_frames < 60) return;
    trc_frames = 0;
    if (!trc_dirty) return;
    trc_dirty = 0;

    printf("[spch] wr=%lu rd=%lu | cmd: la=%lu rb=%lu rbr=%lu spk=%lu ext=%lu rst=%lu | "
           "data=%lu hw=%lu talk=%lu halt=%lu stop=%lu under=%lu smp=%lu | "
           "rom=%lu@%05lX->%02X stat=%02X present=%u\n",
           (unsigned long)trc.writes,        (unsigned long)trc.reads,
           (unsigned long)trc.cmd_loadaddr,  (unsigned long)trc.cmd_readbyte,
           (unsigned long)trc.cmd_readbranch,(unsigned long)trc.cmd_speak,
           (unsigned long)trc.cmd_speakext,  (unsigned long)trc.cmd_reset,
           (unsigned long)trc.data_bytes,    (unsigned long)trc.fifo_hw,
           (unsigned long)trc.talk_starts,   (unsigned long)trc.halts,
           (unsigned long)trc.stop_frames,   (unsigned long)trc.underruns,
           (unsigned long)trc.samples,
           (unsigned long)trc.rom_reads,     (unsigned long)trc.last_rom_addr,
           trc.last_rom_byte, trc.last_status, sp.present);
}
#endif
