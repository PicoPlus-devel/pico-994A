// =====================================================================================
// ti99_host.c - run the TI-99/4A core on Linux and paste a BASIC listing into it.
//
// Boots the console, optionally with a cartridge such as Extended BASIC, types a listing
// in through a port of main.cpp's serial keyboard - the same GROM-derived key table, the
// same column handshake, the same frame counts - then types the commands that follow
// (RUN by default) and prints the screen. Before every ENTER in the listing it rebuilds
// the line being edited from the screen and compares it with what was sent, so a lost
// character is reported as the line it damaged, not as a puzzling error from BASIC later.
//
// Speech is drained at the chip's 8 kHz every frame, as the board's audio path does.
// Without that the chip never finishes talking and CALL SAY waits forever.
//
// The serial keyboard below is a port of serialKeyboardBegin(), serialCharToTIKey() and
// serialKeyboardTick() in main.cpp, which are C++ and tied to the UART, the settings and
// the USB keyboard. A change to one has to be made to the other.
//
// Usage is in hosttest/README.md.
// =====================================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ti99_compat.h"
#include "ti99.h"
#include "ti99_pico.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cpu/tms9918a/tms9918a.h"
#include "cpu/sn76496/sn76496_shim.h"
#include "speech.h"

// The core's headers can route stdio through its SD-card file layer (ti99_fileio.h).
// The harness's own files want the real thing.
#undef FILE
#undef fopen
#undef fread
#undef fwrite
#undef fclose

extern u32 LoopTMS9900(void);

// -------------------------------------------------------------------------------------
// What main.cpp provides to the core on the board
// -------------------------------------------------------------------------------------
void *ti99_psram_alloc(size_t size) { return malloc(size); }
void  ti99_psram_free(void *p)      { free(p); }
void *ti99_mem_alloc(size_t size)   { return malloc(size); }
void  ti99_mem_free(void *p)        { free(p); }
void  TI99RenderLine(u8 y)          { (void)y; }
void  TI99UpdateScreen(void)        { }
void  processDirectAudio(void)      { }
char  __StackLimit;                 // for ti99_sram_free()'s arithmetic; the value is irrelevant

// cassette.c needs FatFs directory calls, and the tape is not what this is for.
void cassette_init(void)                    { }
void cassette_shutdown(void)                { }
void cassette_cru_write(u8 pin, u8 dataBit) { (void)pin; (void)dataBit; }
u8   cassette_read_bit(void)                { return 0; }
void cassette_frame_tick(void)              { }

// -------------------------------------------------------------------------------------
// Serial keyboard - keep the constants and the gap rule in step with main.cpp
// -------------------------------------------------------------------------------------
enum
{
    MIN_HOLD_FRAMES      = 2,
    MIN_GAP_FRAMES       = 1,
    SCAN_TIMEOUT_FRAMES  = 120,
    EDITOR_POLL_SWEEPS   = 2,
    EDITOR_POLL_FRAMES   = 2,
    ENTER_TIMEOUT_FRAMES = 300,
};
enum { SERIAL_IDLE, SERIAL_HOLD, SERIAL_GAP };
typedef struct { u8 key, modifier; } key_combo_t;

static key_combo_t asciiToTI[128];
static u8          keyColumn[TMS_KEY_MAX];
static key_combo_t curKey;
static u8          needCols;
static int         phase = SERIAL_IDLE, phaseFrames, idlePolls;
static unsigned    typed;

// What is typed, in order. Two bytes are not characters but instructions to the harness.
#define SCRIPT_WAIT  0x01           // wait one second
#define SCRIPT_DUMP  0x02           // print the screen
static u8     script[1 << 16];
static size_t scriptLen, scriptPos, lineStart;
static size_t checkUntil;           // lines from here on go to a running program, not the editor
static int    waitFrames;

static long     frame;
static unsigned linesChecked, linesDamaged;

// -t: keyboard sweeps per frame for 1.5 s after each ENTER.
static int    timelineOn, capture;
static char   timeline[128];
static size_t tlLen;
static long   captureFrame;

static void tl_mark(char c) { if (capture && tlLen < sizeof timeline - 1) timeline[tlLen++] = c; }

// The console's own tables say which key produces which character: 48 bytes each at
// 0x1700 (plain), 0x1730 (SHIFT) and 0x1760 (FCTN) in its GROM. See main.cpp.
static int serial_begin(void)
{
    if (MemGROM[0x1705] != 0x0d || MemGROM[0x1706] != 0x20) return -1;

    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            keyColumn[TIKeys[row][col]] = (u8)col;

    static const struct { u16 base; u8 modifier; } tables[] = {
        { 0x1700, TMS_KEY_NONE }, { 0x1730, TMS_KEY_SHIFT }, { 0x1760, TMS_KEY_FUNCTION },
    };
    for (int t = 0; t < 3; t++)
        for (int i = 0; i < 48; i++)
        {
            u8 key = TIKeys[7 - (i % 8)][i / 8];
            u8 ch  = MemGROM[tables[t].base + i];
            if (key == TMS_KEY_NONE || key >= TMS_KEY_JOY1_UP) continue;
            if (ch < 0x20 || ch > 0x7e) continue;
            if (asciiToTI[ch].key != TMS_KEY_NONE) continue;
            asciiToTI[ch].key      = key;
            asciiToTI[ch].modifier = tables[t].modifier;
        }
    return 0;
}

static key_combo_t char_to_key(u8 c)
{
    key_combo_t k = { TMS_KEY_NONE, TMS_KEY_NONE };
    if (c == '\n' || c == '\r') k.key = TMS_KEY_ENTER;
    else if (c == '\t')         k.key = TMS_KEY_SPACE;
    else if (c < 0x80)          k = asciiToTI[c];
    return k;
}

// -------------------------------------------------------------------------------------
// The screen
// -------------------------------------------------------------------------------------
static char screen_char(int row, int col)
{
    int v = pVDPVidMem[row * 32 + col] - 0x60;      // both BASICs offset characters by >60
    return (v >= 0x20 && v < 0x7f) ? (char)v : ' ';
}

static void dump_screen(void)
{
    printf("---- screen at %.1f s ----\n", frame / 60.0);
    for (int row = 0; row < 24; row++)
    {
        char line[33];
        int  end = 0;
        for (int col = 0; col < 32; col++)
            if ((line[col] = screen_char(row, col)) != ' ') end = col + 1;
        line[end] = 0;
        if (end) printf("|%s\n", line);
    }
}

// Just before ENTER: rebuild the line being edited from the screen - the last row with
// the ">" prompt in column 1, plus its continuation rows (columns 2-29, 28 wide) - and
// compare it with what was sent.
static void check_line(size_t end)
{
    char want[512], got[512];
    size_t wn = 0, gn = 0;
    for (size_t i = lineStart; i < end && wn < sizeof want - 1; i++)
        if (script[i] >= 0x20) want[wn++] = (char)script[i];
    while (wn && want[wn - 1] == ' ') wn--;
    want[wn] = 0;

    int top = -1;
    for (int row = 23; row >= 0 && top < 0; row--)
        if (screen_char(row, 1) == '>') top = row;
    if (top < 0) return;

    for (int row = top; row < 24; row++)
        for (int col = 2; col < 30 && gn < sizeof got - 1; col++)
            got[gn++] = screen_char(row, col);
    while (gn && got[gn - 1] == ' ') gn--;
    got[gn] = 0;

    linesChecked++;
    if (strcmp(want, got) != 0)
    {
        linesDamaged++;
        printf("LINE DAMAGED at %.1f s\n  sent: %s\n  got : %s\n", frame / 60.0, want, got);
    }
}

// -------------------------------------------------------------------------------------
// One frame of the serial keyboard, run before the frame as on the board.
//
// KeySweeps is cleared on every call, so it always holds exactly the frame just run. The
// board clears it at each release and on every tick of the gap instead, which is the same
// thing everywhere it is read.
// -------------------------------------------------------------------------------------
static void serial_tick(void)
{
    u8 sweeps = tms9901.KeySweeps;
    tms9901.KeySweeps = 0;

    if (waitFrames > 0) { waitFrames--; return; }

    if (phase == SERIAL_IDLE)
    {
        key_combo_t k = { TMS_KEY_NONE, TMS_KEY_NONE };
        while (scriptPos < scriptLen)
        {
            u8 c = script[scriptPos++];
            if (c == SCRIPT_WAIT) { waitFrames = 60; lineStart = scriptPos; return; }
            if (c == SCRIPT_DUMP) { dump_screen(); lineStart = scriptPos; continue; }
            if (c == '\n' || c == '\r')
            {
                if (lineStart < checkUntil) check_line(scriptPos - 1);
                lineStart = scriptPos;
            }
            k = char_to_key(c);
            if (k.key != TMS_KEY_NONE) break;
        }
        if (k.key == TMS_KEY_NONE) return;

        curKey   = k;
        typed++;
        needCols = (u8)(1u << keyColumn[k.key]);
        if (k.modifier != TMS_KEY_NONE) needCols |= (u8)(1u << keyColumn[k.modifier]);
        phase       = SERIAL_HOLD;
        phaseFrames = 0;
        tms9901.KeyColsScanned = 0;
        if (timelineOn && k.key == TMS_KEY_ENTER) { capture = 90; tlLen = 0; captureFrame = frame; }
        tl_mark('P');
    }

    tms9901.CapsLock = 0;
    int seen = (tms9901.KeyColsScanned & needCols) == needCols;
    phaseFrames++;

    if (phase == SERIAL_HOLD)
    {
        if ((seen && phaseFrames >= MIN_HOLD_FRAMES) || phaseFrames >= SCAN_TIMEOUT_FRAMES)
        {
            phase       = SERIAL_GAP;
            phaseFrames = 0;
            idlePolls   = 0;
            tms9901.KeyColsScanned = 0;
            tl_mark('R');
            return;
        }
        tms9901.Keyboard[curKey.key] = 1;
        if (curKey.modifier != TMS_KEY_NONE) tms9901.Keyboard[curKey.modifier] = 1;
    }
    else
    {
        idlePolls = (sweeps >= EDITOR_POLL_SWEEPS) ? idlePolls + 1 : 0;
        int done = (curKey.key == TMS_KEY_ENTER)
                 ? (idlePolls >= EDITOR_POLL_FRAMES || phaseFrames >= ENTER_TIMEOUT_FRAMES)
                 : ((seen && phaseFrames >= MIN_GAP_FRAMES) || phaseFrames >= SCAN_TIMEOUT_FRAMES);
        if (done)
        {
            phase       = SERIAL_IDLE;
            phaseFrames = 0;
            tl_mark('I');
        }
    }
}

// -------------------------------------------------------------------------------------
// Building the script
// -------------------------------------------------------------------------------------
static void add_byte(u8 b)
{
    if (scriptLen >= sizeof script) { fprintf(stderr, "listing too long\n"); exit(1); }
    script[scriptLen++] = b;
}
static void add(const char *s)   { while (*s) add_byte((u8)*s++); }
static void add_waits(int secs)  { while (secs-- > 0) add_byte(SCRIPT_WAIT); }

// CRLF is one ENTER, as it is on the board.
static void add_listing(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (!(s[i] == '\n' && i && s[i - 1] == '\r')) add_byte((u8)s[i]);
}

// \n ENTER, \w wait a second, \W wait ten, \d print the screen, \\ a backslash.
static void add_commands(const char *s)
{
    for (; *s; s++)
    {
        if (*s != '\\' || !s[1]) { add_byte((u8)*s); continue; }
        switch (*++s)
        {
            case 'n': add_byte('\n');        break;
            case 'w': add_waits(1);          break;
            case 'W': add_waits(10);         break;
            case 'd': add_byte(SCRIPT_DUMP); break;
            default:  add_byte((u8)*s);      break;
        }
    }
}

static void write_wav(const char *path, const s16 *samples, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    int   rate = 8000, bytes = (int)(n * 2), v;
    short fmt[2] = { 1, 1 }, align[2] = { 2, 16 };
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f); v = 16; fwrite(&v, 4, 1, f);
    fwrite(fmt, 2, 2, f); fwrite(&rate, 4, 1, f); v = rate * 2; fwrite(&v, 4, 1, f);
    fwrite(align, 2, 2, f); fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
    fwrite(samples, 2, (size_t)n, f);
    fclose(f);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-t] [-o speech.wav] <cartridge.rpk | -> <listing.bas> [commands]\n"
        "  -            no cartridge: TI BASIC\n"
        "  commands     typed after the listing, default 'RUN\\n\\W'\n"
        "               \\n ENTER  \\w wait 1 s  \\W wait 10 s  \\d print the screen\n"
        "  -t           keyboard sweeps per frame after every ENTER\n"
        "  -o file      write everything spoken to an 8 kHz WAV\n", prog);
}

int main(int argc, char **argv)
{
    const char *wavPath = NULL;
    int opt;
    while ((opt = getopt(argc, argv, "to:")) != -1)
    {
        if      (opt == 't') timelineOn = 1;
        else if (opt == 'o') wavPath = optarg;
        else { usage(argv[0]); return 1; }
    }
    if (argc - optind < 2) { usage(argv[0]); return 1; }
    const char *cart     = argv[optind];
    const char *listing  = argv[optind + 1];
    const char *commands = (argc - optind > 2) ? argv[optind + 2] : "RUN\\n\\W";
    int tiBasic = strcmp(cart, "-") == 0;

    // The machine main.cpp sets up: NTSC, 32K expansion, plain cartridge.
    memset(&myConfig, 0, sizeof myConfig);
    myConfig.machineType = MACH_TYPE_NORMAL32K;
    myConfig.cartType    = CART_TYPE_NORMAL;

    char err[64] = "";
    if (ti99_alloc_memory() || ti99_load_bios(err, sizeof err) ||
        ti99_load_cart(tiBasic ? NULL : cart, 0, err, sizeof err))
    {
        fprintf(stderr, "cannot start the machine: %s\n", err[0] ? err : "out of memory");
        return 1;
    }
    ti99_psg_init(TI99_AUDIO_SAMPLE_RATE);
    if (serial_begin() != 0) { fprintf(stderr, "no key tables in the console GROM\n"); return 1; }

    FILE *f = fopen(listing, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", listing); return 1; }
    static char text[1 << 15];
    size_t n = fread(text, 1, sizeof text, f);
    fclose(f);

    // Title screen, then the menu: TI BASIC is the first entry, a cartridge the second.
    add_waits(3); add(" "); add_waits(2); add(tiBasic ? "1" : "2"); add_waits(5);
    add_byte(SCRIPT_DUMP);
    // Framed as tools/copybas.sh frames it.
    for (int i = 0; i < 5; i++) add("PRINT 0\n");
    add("NEW\n");
    add_waits(1);
    add_listing(text, n);
    add("\n");
    for (int i = 0; i < 5; i++) add("PRINT 0\n");
    add_waits(2);
    checkUntil = scriptLen;
    add_commands(commands);

    static s16 wav[8000 * 60 * 30];                 // 30 minutes
    const long wavMax = (long)(sizeof wav / sizeof wav[0]);
    long wavN = 0, utterance = 0;

    while (scriptPos < scriptLen || phase != SERIAL_IDLE || waitFrames)
    {
        TMS9901_ClearJoyKeyData();
        serial_tick();
        while (LoopTMS9900()) { }

        // The chip's 8 kHz, a frame's worth at a time.
        for (int i = 0; i < 133 && SpeechIsTalking(); i++)
        {
            s16 s = SpeechGetSample();
            if (wavN < wavMax) wav[wavN++] = s;
            utterance++;
        }
        if (utterance && !SpeechIsTalking())
        {
            printf("speech: %ld ms, at %.1f s\n", utterance / 8, frame / 60.0);
            for (int i = 0; i < 1600 && wavN < wavMax; i++) wav[wavN++] = 0;   // 0.2 s apart
            utterance = 0;
        }

        if (capture)
        {
            unsigned sweeps = tms9901.KeySweeps;    // serial_tick cleared it: this frame alone
            tl_mark(sweeps < 10 ? (char)('0' + sweeps) : '+');
            if (--capture == 0)
            {
                timeline[tlLen] = 0;
                printf("ENTER at %.1f s: %s\n", captureFrame / 60.0, timeline);
            }
        }

        if (++frame > 60L * 60 * 60) { printf("gave up after an hour of emulated time\n"); break; }
    }

    dump_screen();
    printf("%u keys typed in %.0f s; %u lines checked, %u damaged\n",
           typed, frame / 60.0, linesChecked, linesDamaged);
    if (wavPath) write_wav(wavPath, wav, wavN);
    return linesDamaged ? 2 : 0;
}
