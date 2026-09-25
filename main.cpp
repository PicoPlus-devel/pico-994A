// =====================================================================================
// pico-994A - Texas Instruments TI-99/4A emulator for RP2350
//
// Front end: video, audio, input, menu and the emulation loop, on top of pico_shared.
// The emulated machine lives in ti99/ (ported from DS994a - see ti99/DS994a-README.md).
//
// Part of the same family as pico-infonesPlus, pico-smsplus, pico-pacPlus,
// pico-pcePlus, pico-genesisPlus, pico-peanutGB and pico_snesPlus.
// =====================================================================================
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "hardware/irq.h"
#include "ff.h"
#include "tusb.h"

#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "recentgames.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "menu_settings.h"

extern "C"
{
#include "ti99_compat.h"
#include "ti99.h"
#include "ti99_pico.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cpu/tms9918a/tms9918a.h"
#include "cpu/sn76496/sn76496_shim.h"
#include "disk.h"
#include "speech.h"
#include "cassette.h"
}

// -------------------------------------------------------------------------------------
// The TI-99/4A's TMS9900 is nominally 3 MHz but spends most of its life waiting on an
// 8-bit multiplexed bus, so the effective rate is well under half a MIPS. 252 MHz is
// ample and is a multiple of 126 MHz, which HSTX builds need for correct HDMI audio
// clocking. Overclock is therefore not offered in the settings menu.
// -------------------------------------------------------------------------------------
#define EMULATOR_CLOCKFREQ_KHZ 252000

#define AUDIOBUFFERSIZE 1024

// The serial keyboard reads the stdio console, so it exists only on boards that have one.
// The SDK defines LIB_PICO_STDIO_UART when pico_enable_stdio_uart() is on, which tracks
// UART_ENABLED in BoardConfigs.cmake (off for HW_CONFIG 11, 12 and 13).
//
// Having a console is not sufficient, though. This is the first thing in the project to
// read the UART's RX pin - stdio only ever transmitted - so a board whose pin map hands
// that pin to something else has always looked perfectly healthy while being physically
// incapable of receiving. HW_CONFIG 1 is exactly that: the Pimoroni DV Demo Base puts the
// second NES controller's clock on GPIO 1, which is UART0 RX, and its own comments say the
// UART should have been disabled for that reason. TX on GPIO 0 is untouched, so printf
// keeps working and only input is dead - which is a miserable thing to debug from the far
// side. Work it out at build time instead and leave the option out where it cannot work.
#if defined(LIB_PICO_STDIO_UART) && defined(PICO_DEFAULT_UART_RX_PIN)
#define SERIAL_RX_PIN_CLAIMED (                                                  \
       PICO_DEFAULT_UART_RX_PIN == NES_PIN_CLK                                   \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_DATA                                  \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_LAT                                   \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_CLK_1                                 \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_DATA_1                                \
    || PICO_DEFAULT_UART_RX_PIN == NES_PIN_LAT_1                                 \
    || PICO_DEFAULT_UART_RX_PIN == WII_PIN_SDA                                   \
    || PICO_DEFAULT_UART_RX_PIN == WII_PIN_SCL                                   \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_CS                                 \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_SCK                                \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_MOSI                               \
    || PICO_DEFAULT_UART_RX_PIN == SDCARD_PIN_MISO)
#define SERIAL_KEYBOARD_AVAILABLE (!SERIAL_RX_PIN_CLAIMED)
#else
#define SERIAL_KEYBOARD_AVAILABLE 0
#endif

#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 1024
#endif
static int g_dvi_audio_gain_q8 = DVI_AUDIO_GAIN_Q8;

static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

bool isFatalError = false;
char *romName;
static bool showSettings = false;
static bool key_done = false;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;

// Marker file that boots the console with no cartridge, straight to TI BASIC.
#define TIBASIC_EXT      ".tib"
#define TIBASIC_MARKER   "TI BASIC.tib"

const int8_t g_settings_visibility_ti99[MOPT_COUNT] = {
    0,                               // Exit Game (always visible in-game)
    1,                               // Reset Game - the TI has no reset button of its own
    BOOTLOADER_BUILD,                // Return to emuLoader picker
    0,                               // Save / Restore State (not ported yet)
    1,                               // Screen Mode
    0,                               // Scanlines toggle (superseded by Screen Mode)
    HSTX,                            // Scanline Type (HSTX only)
    1,                               // FPS Overlay
    0,                               // Audio Enable
    0,                               // Frame Skip
    HSTX && ENABLEDVI,               // Display Mode (HDMI or DVI)
    (EXT_AUDIO_IS_ENABLED),          // External Audio
    1,                               // Font Color
    1,                               // Font Back Color
    ENABLE_VU_METER,                 // VU Meter
    (HW_CONFIG == 8),                // Fruit Jam Volume Control
    0,                               // DMG Palette
    0,                               // Border Mode
    0,                               // Rapid Fire on A
    0,                               // Rapid Fire on B
    0,                               // Auto Insert Disk A (FDS only)
    0,                               // Auto Swap FDS
    0,                               // FDS Disk Swap
    0,                               // Overclock - 252MHz is already plenty for a 3MHz TMS9900
    0,                               // YM Audio (SMS only)
    1,                               // Enter bootsel mode
    1,                               // Controller Test
    0,                               // Recent Games
    0,                               // USB Drive Mode (menu.cpp force-shows this in the rom browser)
    1,                               // Cassette CS1/CS2
    1,                               // Disk DSK1/2/3 (shows N/A without the disk DSR)
    SERIAL_KEYBOARD_AVAILABLE,       // Serial keyboard - hidden where the board has no UART
                                     // console, or gives its RX pin to something else
    0,                               // Sprite Limit (NES only)
    0,                               // Overscan in menu (menu.cpp force-shows this below the menu colors)
};

// -------------------------------------------------------------------------------------
// Cassette deck, exposed to the shared settings menu through the same hook mechanism the
// NES build uses for FDS disk swapping. The menu's mode numbering is TapeMode's, so the
// wrappers exist only to match the hook signatures exactly.
// -------------------------------------------------------------------------------------
static int  cassette_hook_mode()                                     { return (int)cassette_mode(); }
static int  cassette_hook_exists(const char *name, int mode)         { return cassette_name_exists(name, (TapeMode)mode); }
static int  cassette_hook_commit(int index, int mode, const char *n) { return cassette_commit(index, (TapeMode)mode, n); }

static const MenuCassetteHooks cassetteHooks = {
    cassette_num_tapes,
    cassette_tape_name,
    cassette_selected,
    cassette_hook_mode,
    cassette_default_name,
    cassette_hook_exists,
    cassette_hook_commit,
    cassette_rewind,
    cassette_refresh_list,
};

// -------------------------------------------------------------------------------------
// Disk drives, exposed to the settings menu through the same mechanism.
//
// Images sitting next to a cartridge are still auto-mounted by name when it loads (see
// ti99_mount_matching_disks). This is the other half: a folder of loose .dsk images that
// can be put into any drive while the machine is running. TI BASIC needs it most - it
// boots with no cartridge, so there is no name for anything to be auto-mounted against.
// -------------------------------------------------------------------------------------
#define DISK_DIR            "/saves/ti99/disks"
#define DISK_MAX_NAME       25          // 24 characters plus the terminator
#define DISK_MAX_LISTED     64          // images offered in the menu

typedef struct { char name[DISK_MAX_NAME]; } DiskEntry;

static DiskEntry *diskList  = nullptr;  // allocated on first use, then kept
static int        diskCount = 0;

// f_mkdir answers FR_EXIST when the directory is already there, so each level can just
// be asked for unconditionally - same shape as the tape folder.
static void ensure_disk_dir(void)
{
    f_mkdir("/saves");
    f_mkdir("/saves/ti99");
    f_mkdir(DISK_DIR);
}

static bool name_is_dsk(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ".dsk") == 0;
}

static void disk_refresh_list(void)
{
    ensure_disk_dir();

    if (!diskList)
    {
        diskList = (DiskEntry *)malloc(sizeof(DiskEntry) * DISK_MAX_LISTED);
        if (!diskList) { diskCount = 0; return; }
    }
    diskCount = 0;

    DIR dir;
    if (f_opendir(&dir, DISK_DIR) != FR_OK) return;

    FILINFO fno;
    while (diskCount < DISK_MAX_LISTED)
    {
        if (f_readdir(&dir, &fno) != FR_OK || fno.fname[0] == 0) break;
        if (fno.fattrib & (AM_DIR | AM_HID | AM_SYS)) continue;
        if (!name_is_dsk(fno.fname)) continue;

        // Skip rather than truncate: a shortened name would list fine and then fail to
        // open, which is a worse outcome than not offering it.
        if (strlen(fno.fname) >= DISK_MAX_NAME)
        {
            printf("[ti99] disk: ignoring %s (name longer than %d characters)\n",
                   fno.fname, DISK_MAX_NAME - 1);
            continue;
        }

        snprintf(diskList[diskCount++].name, DISK_MAX_NAME, "%s", fno.fname);
    }
    f_closedir(&dir);
}

static int         disk_hook_num_drives()      { return MAX_DSKS; }
static int         disk_hook_num_images()      { return diskCount; }
static const char *disk_hook_image_name(int i) { return (i >= 0 && i < diskCount) ? diskList[i].name : ""; }

static const char *disk_hook_mounted_name(int drive)
{
    if (drive < 0 || drive >= MAX_DSKS || !Disk[drive].isMounted) return nullptr;
    return Disk[drive].filename;
}

// index -1 ejects. disk_mount() announces a failure only by printing onto the emulated
// screen, which is behind the menu, so the outcome is read back from isMounted and
// returned for the menu to report.
static int disk_hook_mount(int drive, int index)
{
    if (drive < 0 || drive >= MAX_DSKS) return -1;

    if (index < 0)
    {
        disk_unmount((u8)drive);
        return 0;
    }
    if (index >= diskCount || !diskList) return -1;

    // disk_mount() takes both halves writable, so hand it copies rather than literals.
    char dir[] = DISK_DIR;
    char name[DISK_MAX_NAME];
    snprintf(name, sizeof(name), "%s", diskList[index].name);

    disk_mount((u8)drive, dir, name);
    return Disk[drive].isMounted ? 0 : -1;
}

// -------------------------------------------------------------------------------------
// Making a blank disk.
//
// disk.c has a disk_create_blank() of its own, but it writes a fixed BLANK_A..Z.DSK name
// into FatFS's current directory - and this port deliberately never sets one, building
// absolute paths instead - so the image is built here.
//
// A v9t9 image is just the sectors, so formatting is sector 0 (the volume header and the
// allocation bitmap), sector 1 (the file index, empty), and 0xE5 to the end. The geometry
// below is 360K DSDD: 1440 sectors, 40 tracks, 18 sectors per track, two sides. That is
// the largest the TI Disk Controller DSR handles and the largest disk.c accepts
// (MAX_DSK_SECTORS), so it is the one size worth offering.
// -------------------------------------------------------------------------------------
#define DISK_NEW_SECTORS    1440        // 360 KB
#define DISK_NEW_BYTES      (DISK_NEW_SECTORS * 256)

// TI volume names are upper case and hold no spaces or dots. Anything else the user types
// becomes an underscore rather than being rejected - the name is a label, not a key.
static void disk_sanitise_name(char *dst, size_t dstsize, const char *src)
{
    size_t n = 0;
    for (; src && *src && n < dstsize - 1; src++)
    {
        char c = *src;
        if (c >= 'a' && c <= 'z') c -= 32;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        dst[n++] = ok ? c : '_';
    }
    dst[n] = 0;
    if (n == 0) snprintf(dst, dstsize, "NEWDISK");
}

static void disk_image_path(char *dst, size_t dstsize, const char *name)
{
    snprintf(dst, dstsize, "%s/%s.dsk", DISK_DIR, name);
}

// Pre-fills the name field with the first DISKnn nobody has used yet, which is also the
// name that gets used as-is when there is no keyboard to edit it with.
static void disk_hook_default_name(char *buf, size_t n)
{
    ensure_disk_dir();
    for (int i = 1; i < 100; i++)
    {
        char cand[DISK_LABEL_MAX];
        snprintf(cand, sizeof(cand), "DISK%02d", i);

        char path[MAX_PATH];
        disk_image_path(path, sizeof(path), cand);

        FILINFO fno;
        if (f_stat(path, &fno) != FR_OK) { snprintf(buf, n, "%s", cand); return; }
    }
    snprintf(buf, n, "NEWDISK");
}

static int disk_hook_name_exists(const char *name)
{
    char clean[DISK_LABEL_MAX];
    disk_sanitise_name(clean, sizeof(clean), name);

    char path[MAX_PATH];
    disk_image_path(path, sizeof(path), clean);

    FILINFO fno;
    return (f_stat(path, &fno) == FR_OK) ? 1 : 0;
}

// Formats a blank image, rescans, and answers with its place in the list so the menu can
// put it straight into the drive. -1 if anything went wrong.
static int disk_hook_create(const char *name)
{
    char clean[DISK_LABEL_MAX];
    disk_sanitise_name(clean, sizeof(clean), name);

    ensure_disk_dir();

    char path[MAX_PATH];
    disk_image_path(path, sizeof(path), clean);

    FIL fil;
    if (f_open(&fil, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return -1;

    u8  sector[256];
    UINT bw;
    bool ok = true;

    // --- sector 0: volume header --------------------------------------------------
    memset(sector, 0x00, sizeof(sector));
    memset(sector, ' ', 10);                                // name, space padded to 10
    memcpy(sector, clean, strlen(clean) > 10 ? 10 : strlen(clean));
    sector[10] = (DISK_NEW_SECTORS >> 8) & 0xFF;            // total sectors, big endian
    sector[11] = DISK_NEW_SECTORS & 0xFF;
    sector[12] = 18;                                        // sectors per track
    sector[13] = 'D'; sector[14] = 'S'; sector[15] = 'K';
    sector[16] = ' ';                                       // not write protected
    sector[17] = 40;                                        // tracks per side
    sector[18] = 2;                                         // sides
    sector[19] = 2;                                         // double density

    // Allocation bitmap from byte 0x38, one bit per sector, lowest sector in the low bit.
    // Sectors 0 and 1 are the header and the file index, so they are taken from the
    // start; the tail past the last real sector is filled in so nothing is handed out.
    sector[0x38] = 0x03;
    for (size_t i = 0x38 + (DISK_NEW_SECTORS / 8); i < sizeof(sector); i++) sector[i] = 0xFF;

    if (f_write(&fil, sector, sizeof(sector), &bw) != FR_OK || bw != sizeof(sector)) ok = false;

    // --- sector 1: file descriptor index, empty -----------------------------------
    memset(sector, 0x00, sizeof(sector));
    if (ok && (f_write(&fil, sector, sizeof(sector), &bw) != FR_OK || bw != sizeof(sector))) ok = false;

    // --- the rest: 0xE5, which is what an unwritten TI sector reads as -------------
    memset(sector, 0xE5, sizeof(sector));
    for (int i = 2; ok && i < DISK_NEW_SECTORS; i++)
    {
        if (f_write(&fil, sector, sizeof(sector), &bw) != FR_OK || bw != sizeof(sector)) ok = false;
    }
    f_close(&fil);

    if (!ok)
    {
        f_unlink(path);                 // a half written image is worse than none
        printf("[ti99] disk: could not write %s\n", path);
        return -1;
    }
    printf("[ti99] disk: created %s (%d sectors)\n", path, DISK_NEW_SECTORS);

    disk_refresh_list();
    for (int i = 0; i < diskCount; i++)
    {
        const char *dot = strrchr(diskList[i].name, '.');
        size_t stem = dot ? (size_t)(dot - diskList[i].name) : strlen(diskList[i].name);
        if (stem == strlen(clean) && strncasecmp(diskList[i].name, clean, stem) == 0) return i;
    }
    return -1;
}

static const MenuDiskHooks diskHooks = {
    disk_hook_num_drives,
    disk_hook_num_images,
    disk_hook_image_name,
    disk_hook_mounted_name,
    disk_hook_mount,
    disk_refresh_list,
    disk_hook_default_name,
    disk_hook_name_exists,
    disk_hook_create,
};

// 256x192 is a 4:3 picture already; the 8:7 modes would stretch it wrongly.
const uint8_t g_available_screen_modes_ti99[] = {
    0, // SCANLINE_8_7
    0, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1  // NOSCANLINE_1_1
};

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static uint16_t wiipad_raw_cached = 0;
#endif

static DWORD prevButtons[2]{};

// -------------------------------------------------------------------------------------
// PSRAM hooks used by the SAMS expansion and by oversized cartridges (ti99.h).
// Returns NULL on boards with no PSRAM, which the core reads as "not available".
// -------------------------------------------------------------------------------------
extern "C" void *ti99_psram_alloc(size_t size)
{
    if (!Frens::isPsramEnabled()) return nullptr;
    return Frens::f_malloc(size);
}

extern "C" void ti99_psram_free(void *p)
{
    if (p) Frens::f_free(p);
}

// -------------------------------------------------------------------------------------
// Preferred-PSRAM allocation: PSRAM when the board has it, SRAM otherwise. Used for the
// buffers the machine cannot run without, so no board is excluded - unlike
// ti99_psram_alloc above, which returns NULL to gate optional features off.
// -------------------------------------------------------------------------------------
extern "C" void *ti99_mem_alloc(size_t size)
{
    return Frens::f_malloc(size);
}

extern "C" void ti99_mem_free(void *p)
{
    if (p) Frens::f_free(p);
}

// =====================================================================================
// Video
//
// The VDP renders scanlines into XBuf as 8-bit palette indices (256x192). At end of
// frame Loop9918 calls TI99UpdateScreen(), which only flags the frame as ready; the
// conversion to display pixels happens in processPerFrame() right after the pace wait,
// i.e. at the start of vblank. Doing it there rather than mid-frame means DMA never
// reads a half-converted line - the same reason pico-pacPlus defers its copy.
// =====================================================================================
static WORD ti99_palette[16];        // TI palette -> display pixel format

static void build_palette(void)
{
    for (int i = 0; i < 16; i++)
    {
        uint8_t r = TMS9918A_palette[i * 3 + 0];
        uint8_t g = TMS9918A_palette[i * 3 + 1];
        uint8_t b = TMS9918A_palette[i * 3 + 2];
#if HSTX
        // HSTX framebuffer is RGB555
        ti99_palette[i] = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
#else
        // PicoDVI framebuffer is RGB444 in the low 12 bits
        ti99_palette[i] = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
#endif
    }
}

// One finished scanline: convert it into the matching framebuffer row, and paint the
// side borders in the backdrop colour the machine is showing right now. On line 0 the
// top and bottom margins (and the FPS digits) are painted too, so the whole 320x240
// area is refreshed once per frame.
static inline WORD *get_display_line(int y)
{
#if HSTX
    return hstx_getlineFromFramebuffer(y);
#else
    return &Frens::framebuffer[y * 320];
#endif
}

// Top and bottom margins take the backdrop colour, so the picture sits inside a border
// the same colour the machine itself shows. Repainted once per frame (on line 0) since
// a title can change the backdrop between frames.
static void __not_in_flash_func(paint_margins)(WORD backdrop)
{
    for (int y = 0; y < TI99_MARGIN_TOP; y++)
    {
        WORD *top = get_display_line(y);
        WORD *bot = get_display_line(240 - 1 - y);
        for (int x = 0; x < 320; x++) { top[x] = backdrop; bot[x] = backdrop; }
    }

    // FPS digits live in the top margin, clear of the picture, so they go on after it.
    if (settings.flags.displayFrameRate)
    {
        WORD fgc = ti99_palette[15];
        WORD bgc = ti99_palette[1];
        char d0 = (char)('0' + ((fps / 10) % 10));
        char d1 = (char)('0' + (fps % 10));
        for (int row = 0; row < 8; row++)
        {
            WORD *dst = get_display_line(8 + row) + 4;
            char s0 = getcharslicefrom8x8font(d0, row);
            char s1 = getcharslicefrom8x8font(d1, row);
            for (int b = 0; b < 8; b++) { *dst++ = (s0 & 1) ? fgc : bgc; s0 >>= 1; }
            for (int b = 0; b < 8; b++) { *dst++ = (s1 & 1) ? fgc : bgc; s1 >>= 1; }
        }
    }
}

extern "C" void __not_in_flash_func(TI99RenderLine)(u8 y)
{
    if (y >= TI99_SCREEN_H) return;

    const WORD backdrop = ti99_palette[ti99_backdrop_color & 0x0F];

    if (y == 0) paint_margins(backdrop);

    WORD *dst = get_display_line(y + TI99_MARGIN_TOP);
    const u8 *src = XBuf;
    WORD *px = dst + TI99_MARGIN_LEFT;

    for (int x = 0; x < TI99_SCREEN_W; x++)
    {
        u8 c = src[x];
        // Colour 0 is transparent on the 9918A and shows the backdrop selected in
        // VDP register 7.
        px[x] = c ? ti99_palette[c & 0x0F] : backdrop;
    }

    for (int x = 0; x < TI99_MARGIN_LEFT; x++)
    {
        dst[x] = backdrop;
        dst[320 - 1 - x] = backdrop;
    }
}

extern "C" void TI99UpdateScreen(void)
{
    if (settings.flags.displayFrameRate)
    {
        uint32_t tick_us = Frens::time_us() - start_tick_us;
        if (tick_us) fps = (1000000 - 1) / tick_us + 1;
        start_tick_us = Frens::time_us();
    }
}

// Per-scanline audio sampling is an accuracy option upstream (sounddriver == 2). This
// port mixes a whole frame at once in process_audio_frame, so nothing to do here.
extern "C" void processDirectAudio(void) { }

// =====================================================================================
// Audio
//
// One frame of PSG output per emulated frame, pushed through whichever sink this board
// has: the Fruit Jam line-out codec, the PicoDVI HDMI ring, or the HSTX data islands.
// =====================================================================================
static inline int16_t apply_dvi_gain_i32(int x)
{
    int64_t v = (int64_t)x * (int64_t)g_dvi_audio_gain_q8;
    v >>= 8;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static s16 audioFrameBuf[TI99_AUDIO_SAMPLE_RATE / 50 + 2];   // worst case is PAL (882)
static s16 tapeFrameBuf[TI99_AUDIO_SAMPLE_RATE / 50 + 2];    // cassette monitor, same shape

// How loud the tape sits in the mix, as a fraction of full scale in Q15. About -20dB,
// which is roughly where the speech synthesiser sits - audible as feedback without
// eating the headroom the PSG needs.
#define TAPE_MONITOR_LEVEL  3200

// -------------------------------------------------------------------------------------
// The Speech Synthesizer runs its filter at 8kHz, so its output is stepped through with
// a 16.16 phase accumulator and linearly interpolated up to the output rate. Doing it
// per output sample rather than resampling a block keeps the state tiny and means the
// speech stays in step even when a frame runs long.
// -------------------------------------------------------------------------------------
#define SPEECH_PHASE_STEP  (((uint32_t)TI99_SPEECH_RATE << 16) / TI99_AUDIO_SAMPLE_RATE)

static uint32_t speechPhase = 0;
static int      speechPrev = 0, speechNext = 0;

static inline int __not_in_flash_func(speech_next_sample)(void)
{
    speechPhase += SPEECH_PHASE_STEP;
    while (speechPhase >= (1u << 16))
    {
        speechPhase -= (1u << 16);
        speechPrev = speechNext;
        speechNext = SpeechGetSample();
    }
    // Linear interpolation between the two 8kHz samples either side of us.
    int frac = (int)(speechPhase & 0xFFFF);
    return speechPrev + (((speechNext - speechPrev) * frac) >> 16);
}

static inline int16_t __not_in_flash_func(mix_clamp)(int a, int b)
{
    int v = a + b;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void __not_in_flash_func(process_audio_frame)(void)
{
    const int samples = myConfig.isPAL ? (TI99_AUDIO_SAMPLE_RATE / 50)
                                       : (TI99_AUDIO_SAMPLE_RATE / 60);

    ti99_psg_mix(audioFrameBuf, samples);

    // Fold the speech synthesiser in on top of the PSG. Skipped entirely when the chip
    // is idle, which is nearly all of the time.
    if (SpeechIsTalking())
    {
        for (int i = 0; i < samples; i++)
            audioFrameBuf[i] = mix_clamp(audioFrameBuf[i], speech_next_sample());
    }
    else
    {
        speechPhase = 0;
        speechPrev = speechNext = 0;
    }

    // Cassette monitor. On a real console the DSR opens the audio gate during a load,
    // which is why you hear the tape through the TV - and it is the only sign a cassette
    // load gives that it is getting anywhere. Kept well down: the 4x output gain leaves
    // little headroom (see CHANGELOG), and during a load the PSG is silent anyway.
    if (cassette_monitor_active())
    {
        cassette_monitor_fill(tapeFrameBuf, samples);
        for (int i = 0; i < samples; i++)
        {
            int t = ((int)tapeFrameBuf[i] * TAPE_MONITOR_LEVEL) >> 15;
            audioFrameBuf[i] = mix_clamp(audioFrameBuf[i], t);
        }
    }

#if HSTX
#if EXT_AUDIO_IS_ENABLED
    bool audioJackConnected = Frens::isHeadPhoneJackConnected();
#endif
    for (int i = 0; i < samples; i++)
    {
        int l = audioFrameBuf[i];
        int r = l;
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter) addSampleToVUMeter(l);
#endif
#if EXT_AUDIO_IS_ENABLED
        if (settings.flags.useExtAudio || audioJackConnected)
        {
            EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
            continue;
        }
#endif
        hstx_push_audio_sample(apply_dvi_gain_i32(l), apply_dvi_gain_i32(r));
    }
#else
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio)
    {
        for (int i = 0; i < samples; i++)
        {
            int s = audioFrameBuf[i];
            EXT_AUDIO_ENQUEUE_SAMPLE(s, s);
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter) addSampleToVUMeter(s);
#endif
        }
        return;
    }
#endif
    {
        auto &ring = dvi_->getAudioRingBuffer();
        for (int i = 0; i < samples; i++)
        {
            int l = apply_dvi_gain_i32(audioFrameBuf[i]);
            if (ring.getWritableSize() > 0)
            {
                auto p = ring.getWritePointer();
                *p = {static_cast<short>(l), static_cast<short>(l)};
                ring.advanceWritePointer(1);
            }
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter) addSampleToVUMeter(l);
#endif
        }
    }
#endif
}

// =====================================================================================
// Input
//
// The TMS9901 exposes keyboard and joysticks through one flat array - tms9901.Keyboard[]
// indexed by TMS_KEY_* - which the CRU read decodes by column (tms9901.c). So driving
// the real machine is just a matter of setting the right entries each frame.
//
// The TI keyboard has 48 keys and reaches everything else through FCTN and SHIFT
// combinations. All 48 are mapped directly below; on top of that, keys a PC keyboard
// has but the TI does not (arrows, backspace, delete, escape, function keys, minus)
// are synthesised as the FCTN/SHIFT combination a TI user would type.
//
// The console settles what each combination produces, and says so in its own GROM: three
// 48-byte translation tables sit at 0x1700 (plain), 0x1730 (SHIFT) and 0x1760 (FCTN) in
// 994aGROM.bin, one entry per key in scan order. That is the reference for everything
// below - the TI Extended BASIC manual in assets/ is not, since it documents the earlier
// TI-99/4, which had no FCTN key and put the cursor keys on SHIFT.
// =====================================================================================

// A TI keypress, optionally with a modifier the TI itself would require.
struct TIKeyCombo
{
    u8   key;
    u8   modifier = TMS_KEY_NONE;   // TMS_KEY_NONE, TMS_KEY_FUNCTION or TMS_KEY_SHIFT
    bool exact    = false;          // modifier above is the whole story: do not also pass
                                    // through the SHIFT the user is physically holding
};

static TIKeyCombo hidKeyToTIKey(uint8_t hid, bool shifted)
{
    // A PC keyboard reaches these with SHIFT; the TI keeps them on the FCTN layer of a
    // different key entirely, so the scancode alone cannot say which character is meant.
    // They are marked exact because the TI wants FCTN+key and nothing else, so the held
    // SHIFT is not passed through on top. Everything a US keyboard and the TI shift alike
    // (!@#$%^&*() : < > +) falls through to the plain mapping below and needs nothing
    // special.
    if (shifted)
    {
        switch (hid)
        {
            case HID_KEY_APOSTROPHE:    return {TMS_KEY_P, TMS_KEY_FUNCTION, true}; // '"' is FCTN+P
            case HID_KEY_MINUS:         return {TMS_KEY_U, TMS_KEY_FUNCTION, true}; // '_' is FCTN+U
            case HID_KEY_BRACKET_LEFT:  return {TMS_KEY_F, TMS_KEY_FUNCTION, true}; // '{' is FCTN+F
            case HID_KEY_BRACKET_RIGHT: return {TMS_KEY_G, TMS_KEY_FUNCTION, true}; // '}' is FCTN+G
            case HID_KEY_BACKSLASH:     return {TMS_KEY_A, TMS_KEY_FUNCTION, true}; // '|' is FCTN+A
            case HID_KEY_SLASH:         return {TMS_KEY_I, TMS_KEY_FUNCTION, true}; // '?' is FCTN+I
            case HID_KEY_GRAVE:         return {TMS_KEY_W, TMS_KEY_FUNCTION, true}; // '~' is FCTN+W
            default: break;
        }
    }

    switch (hid)
    {
        // --- letters ---------------------------------------------------------------
        case HID_KEY_A: return {TMS_KEY_A, TMS_KEY_NONE};
        case HID_KEY_B: return {TMS_KEY_B, TMS_KEY_NONE};
        case HID_KEY_C: return {TMS_KEY_C, TMS_KEY_NONE};
        case HID_KEY_D: return {TMS_KEY_D, TMS_KEY_NONE};
        case HID_KEY_E: return {TMS_KEY_E, TMS_KEY_NONE};
        case HID_KEY_F: return {TMS_KEY_F, TMS_KEY_NONE};
        case HID_KEY_G: return {TMS_KEY_G, TMS_KEY_NONE};
        case HID_KEY_H: return {TMS_KEY_H, TMS_KEY_NONE};
        case HID_KEY_I: return {TMS_KEY_I, TMS_KEY_NONE};
        case HID_KEY_J: return {TMS_KEY_J, TMS_KEY_NONE};
        case HID_KEY_K: return {TMS_KEY_K, TMS_KEY_NONE};
        case HID_KEY_L: return {TMS_KEY_L, TMS_KEY_NONE};
        case HID_KEY_M: return {TMS_KEY_M, TMS_KEY_NONE};
        case HID_KEY_N: return {TMS_KEY_N, TMS_KEY_NONE};
        case HID_KEY_O: return {TMS_KEY_O, TMS_KEY_NONE};
        case HID_KEY_P: return {TMS_KEY_P, TMS_KEY_NONE};
        case HID_KEY_Q: return {TMS_KEY_Q, TMS_KEY_NONE};
        case HID_KEY_R: return {TMS_KEY_R, TMS_KEY_NONE};
        case HID_KEY_S: return {TMS_KEY_S, TMS_KEY_NONE};
        case HID_KEY_T: return {TMS_KEY_T, TMS_KEY_NONE};
        case HID_KEY_U: return {TMS_KEY_U, TMS_KEY_NONE};
        case HID_KEY_V: return {TMS_KEY_V, TMS_KEY_NONE};
        case HID_KEY_W: return {TMS_KEY_W, TMS_KEY_NONE};
        case HID_KEY_X: return {TMS_KEY_X, TMS_KEY_NONE};
        case HID_KEY_Y: return {TMS_KEY_Y, TMS_KEY_NONE};
        case HID_KEY_Z: return {TMS_KEY_Z, TMS_KEY_NONE};

        // --- digits ----------------------------------------------------------------
        // The TI's shifted number row matches a US PC keyboard exactly (!@#$%^&*()),
        // so these need no translation.
        case HID_KEY_1: return {TMS_KEY_1, TMS_KEY_NONE};
        case HID_KEY_2: return {TMS_KEY_2, TMS_KEY_NONE};
        case HID_KEY_3: return {TMS_KEY_3, TMS_KEY_NONE};
        case HID_KEY_4: return {TMS_KEY_4, TMS_KEY_NONE};
        case HID_KEY_5: return {TMS_KEY_5, TMS_KEY_NONE};
        case HID_KEY_6: return {TMS_KEY_6, TMS_KEY_NONE};
        case HID_KEY_7: return {TMS_KEY_7, TMS_KEY_NONE};
        case HID_KEY_8: return {TMS_KEY_8, TMS_KEY_NONE};
        case HID_KEY_9: return {TMS_KEY_9, TMS_KEY_NONE};
        case HID_KEY_0: return {TMS_KEY_0, TMS_KEY_NONE};

        // --- the rest of the real TI keyboard ---------------------------------------
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER:  return {TMS_KEY_ENTER,  TMS_KEY_NONE};
        case HID_KEY_SPACE:         return {TMS_KEY_SPACE,  TMS_KEY_NONE};
        case HID_KEY_PERIOD:        return {TMS_KEY_PERIOD, TMS_KEY_NONE};
        case HID_KEY_COMMA:         return {TMS_KEY_COMMA,  TMS_KEY_NONE};
        case HID_KEY_SLASH:         return {TMS_KEY_SLASH,  TMS_KEY_NONE};
        case HID_KEY_SEMICOLON:     return {TMS_KEY_SEMI,   TMS_KEY_NONE};
        case HID_KEY_EQUAL:         return {TMS_KEY_EQUALS, TMS_KEY_NONE};

        // --- synthesised: cursor movement -------------------------------------------
        // On a TI these are FCTN+E/S/D/X, which is also what games watch for.
        case HID_KEY_ARROW_UP:      return {TMS_KEY_E, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_DOWN:    return {TMS_KEY_X, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_LEFT:    return {TMS_KEY_S, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_RIGHT:   return {TMS_KEY_D, TMS_KEY_FUNCTION};

        // --- synthesised: editing keys ----------------------------------------------
        case HID_KEY_BACKSPACE:     return {TMS_KEY_S, TMS_KEY_FUNCTION};   // FCTN+S is the TI's backspace
        case HID_KEY_DELETE:        return {TMS_KEY_1, TMS_KEY_FUNCTION};   // FCTN+1 = DEL
        case HID_KEY_INSERT:        return {TMS_KEY_2, TMS_KEY_FUNCTION};   // FCTN+2 = INS
        case HID_KEY_ESCAPE:        return {TMS_KEY_9, TMS_KEY_FUNCTION};   // FCTN+9 = BACK (quit)

        // --- synthesised: the FCTN number row, on the PC function keys ---------------
        case HID_KEY_F1:            return {TMS_KEY_1, TMS_KEY_FUNCTION};   // DEL
        case HID_KEY_F2:            return {TMS_KEY_2, TMS_KEY_FUNCTION};   // INS
        case HID_KEY_F3:            return {TMS_KEY_3, TMS_KEY_FUNCTION};   // ERASE
        case HID_KEY_F4:            return {TMS_KEY_4, TMS_KEY_FUNCTION};   // CLEAR
        case HID_KEY_F5:            return {TMS_KEY_5, TMS_KEY_FUNCTION};   // BEGIN
        case HID_KEY_F6:            return {TMS_KEY_6, TMS_KEY_FUNCTION};   // PROC'D
        case HID_KEY_F7:            return {TMS_KEY_7, TMS_KEY_FUNCTION};   // AID
        case HID_KEY_F8:            return {TMS_KEY_8, TMS_KEY_FUNCTION};   // REDO
        case HID_KEY_F9:            return {TMS_KEY_9, TMS_KEY_FUNCTION};   // BACK
        case HID_KEY_F10:           return {TMS_KEY_EQUALS, TMS_KEY_FUNCTION}; // QUIT

        // --- synthesised: characters the TI puts elsewhere ---------------------------
        case HID_KEY_MINUS:         return {TMS_KEY_SLASH,  TMS_KEY_SHIFT};    // '-' is SHIFT+/
        case HID_KEY_APOSTROPHE:    return {TMS_KEY_O,      TMS_KEY_FUNCTION}; // '\'' is FCTN+O
        case HID_KEY_BRACKET_LEFT:  return {TMS_KEY_R,      TMS_KEY_FUNCTION}; // '[' is FCTN+R
        case HID_KEY_BRACKET_RIGHT: return {TMS_KEY_T,      TMS_KEY_FUNCTION}; // ']' is FCTN+T
        case HID_KEY_BACKSLASH:     return {TMS_KEY_Z,      TMS_KEY_FUNCTION}; // '\\' is FCTN+Z
        case HID_KEY_GRAVE:         return {TMS_KEY_C,      TMS_KEY_FUNCTION}; // '`' is FCTN+C

        default:                    return {TMS_KEY_NONE, TMS_KEY_NONE};
    }
}

// Alpha Lock is a physical latching key on the TI, so a PC Caps Lock press toggles it
// rather than holding it. Tracked here because HID reports the LED state as a modifier
// only on some keyboards.
static bool alphaLock = false;
static bool capsWasDown = false;

// A key keeps the TI combination it was pressed as until it is released, as a PC settles
// the character at key-down. For the characters the TI keeps on FCTN, SHIFT decides which
// TI key is down at all: letting go of SHIFT a moment before the key - ordinary typing -
// would swap FCTN+P for FCTN+O under a key still held, and KSCAN takes that as a second
// keystroke, so Shift+' typed "' rather than ".
static uint8_t    heldHid[6];
static TIKeyCombo heldCombo[6];

static void update_ti_keyboard(void)
{
    TMS9901_ClearJoyKeyData();

    const auto &kb = io::getCurrentKeyboardState();

    // Modifiers the user is physically holding. SHIFT is held back until the keys have
    // been looked at: a key that resolves to an exact TI combination supplies its own
    // modifier and must not have the held SHIFT added on top.
    bool shiftHeld = (kb.modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT)) != 0;

    if (kb.modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        tms9901.Keyboard[TMS_KEY_CONTROL] = 1;
    if (kb.modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        tms9901.Keyboard[TMS_KEY_FUNCTION] = 1;

    bool capsDown      = false;
    bool suppressShift = false;
    uint8_t    nowHid[6] = {};
    TIKeyCombo nowCombo[6];
    for (int i = 0; i < 6; i++)
    {
        uint8_t hid = kb.keycode[i];
        if (!hid) continue;

        if (hid == HID_KEY_CAPS_LOCK) { capsDown = true; continue; }

        TIKeyCombo k = hidKeyToTIKey(hid, shiftHeld);
        for (int j = 0; j < 6; j++)
            if (heldHid[j] == hid) { k = heldCombo[j]; break; }
        nowHid[i]   = hid;
        nowCombo[i] = k;

        if (k.key != TMS_KEY_NONE)
        {
            tms9901.Keyboard[k.key] = 1;
            if (k.modifier != TMS_KEY_NONE) tms9901.Keyboard[k.modifier] = 1;
            if (k.exact) suppressShift = true;
        }
    }
    memcpy(heldHid, nowHid, sizeof(heldHid));
    memcpy(heldCombo, nowCombo, sizeof(heldCombo));

    if (shiftHeld && !suppressShift) tms9901.Keyboard[TMS_KEY_SHIFT] = 1;

    if (capsDown && !capsWasDown) alphaLock = !alphaLock;
    capsWasDown = capsDown;
    tms9901.CapsLock = alphaLock ? 1 : 0;
}

// -------------------------------------------------------------------------------------
// Serial keyboard: paste text straight into TI BASIC over the UART console.
//
// Characters arriving on stdin are turned into TI keypresses and held in
// tms9901.Keyboard[] long enough for the console's KSCAN to notice them, so a BASIC
// listing pasted into a terminal types itself in.
//
// Two rates are in play and they are three orders of magnitude apart: at 115200 baud the
// UART delivers ~11500 characters a second and the console can absorb about twelve.
// Everything below exists to bridge that - a ring buffer, XON/XOFF to hold the sender
// off, and a drain that runs far more often than once a frame.
//
// UART only. The native USB port is the *host* port on boards without PIO USB, and USB
// drive mode already owns the device stack when there is one, so there is no CDC to use.
// -------------------------------------------------------------------------------------
#if SERIAL_KEYBOARD_AVAILABLE

// A keypress is held until the console has actually read the columns the key and its
// modifier sit in - not for a fixed number of frames. KSCAN stops entirely while BASIC
// tokenises a line or scrolls the screen, and typing into that window is how a paste
// loses whole lines. Waiting on the columns rather than on a full 0-5 sweep also keeps
// this working in the split-keyboard scan modes, which never read all six.
//
// The timeout is a backstop against software that never scans at all (a game ignoring the
// keyboard), not a pacing mechanism, so it is deliberately far longer than any scroll.
// Mid-line characters land reliably on this handshake alone, so these stay tight: making
// them generous would only halve the paste rate for no gain.
static constexpr int MIN_HOLD_FRAMES      = 2;
static constexpr int MIN_GAP_FRAMES       = 1;
static constexpr int SCAN_TIMEOUT_FRAMES  = 120;
// After ENTER, BASIC tokenises the line and scrolls, and somewhere in that work it makes a
// keyboard sweep of its own that is not the editor asking for input. A key held down at
// that moment is read and thrown away, and the column handshake cannot tell the difference.
// That is how a paste lost the first character of a line - usually a line-number digit, so
// the line was quietly renumbered and broke the program somewhere else entirely. After
// ENTER the next key therefore waits until the editor is polling at its own rate: several
// full sweeps a frame (tms9901.KeySweeps), in consecutive frames with no key held.
//
// Measured on the host by pasting listings into Extended BASIC (90 ENTERs) and TI BASIC
// (54): while BASIC was busy a frame showed 0 or 1 sweeps, never more; once the editor was
// polling, 2-9. The wait ran from 9 to 47 frames and was longest late in a long listing,
// so the fixed 30-frame gap this replaces was too long for most lines and too short for
// the rest.
static constexpr int EDITOR_POLL_SWEEPS   = 2;    // sweeps in one frame that mean the editor is polling
static constexpr int EDITOR_POLL_FRAMES   = 2;    // consecutive such frames: margin over a lone busy sweep
// Backstop for software that never polls that fast, such as a running program reading
// keys with CALL KEY. Longer than SCAN_TIMEOUT_FRAMES because BASIC's busy time after
// ENTER keeps growing with the program.
static constexpr int ENTER_TIMEOUT_FRAMES = 300;

// After abandoning a paste, ignore everything arriving until the line has been quiet this
// long. Releasing XOFF lets the host empty a transmit queue that may still hold most of
// the old file, and without this that stale text simply types itself into the next paste.
static constexpr uint32_t SERIAL_DISCARD_QUIET_US = 500000;

// Big enough to swallow a whole listing without ever asking the sender to stop. That is
// the point: XOFF is not free. Stopping a sender that is about to finish can strand the
// last bytes of the file in its transmit queue, where they are lost if the writing process
// closes the port while still held off - which is exactly how a paste came to end 20 bytes
// short of the end. Flow control is kept as a backstop for listings larger than this, but
// with the threshold high enough that ordinary ones never reach it.
// Power of two - the ring indices are masked.
static constexpr unsigned SERIAL_RING_SIZE = 16384;
static constexpr unsigned SERIAL_XOFF_USED = (SERIAL_RING_SIZE * 3) / 4;  // hold the sender off here
static constexpr unsigned SERIAL_XON_USED  = SERIAL_RING_SIZE / 4;        // release it once drained to here

static constexpr uint8_t ASCII_ETX  = 0x03;   // Ctrl-C: abandon the paste
static constexpr uint8_t ASCII_XON  = 0x11;
static constexpr uint8_t ASCII_XOFF = 0x13;

enum SerialPhase { SERIAL_IDLE, SERIAL_HOLD, SERIAL_GAP };

static uint8_t    serialRing[SERIAL_RING_SIZE];
// head is advanced by the UART interrupt, tail by the frame loop - single producer,
// single consumer, and both indices are aligned words, so no locking is needed.
static volatile unsigned serialHead = 0, serialTail = 0;   // head == tail is empty
static uint8_t    serialPrevByte = 0;          // interrupt side only
static volatile bool serialCancelReq = false;  // Ctrl-C seen by the interrupt
static volatile bool     serialDiscarding = false;  // swallowing the host's leftover queue
static volatile uint32_t serialDiscardUntil = 0;
// Counted so a finished paste can report itself. If this total falls short of the file
// that was sent, the characters were lost in transit and never reached the board at all -
// which is a different fault from anything the typing side can cause.
static volatile uint32_t serialRxCount = 0;
static volatile uint32_t serialLastRxUs = 0;
static uint32_t serialTypedCount = 0;
static bool     serialXoffUsed = false;
static volatile bool serialOverflowed = false;
static bool       serialOverflowWarned = false;
static bool       serialXoffSent = false;
static bool       serialLutReady = false;
static TIKeyCombo serialAsciiToTI[128];
static uint8_t    serialKeyColumn[TMS_KEY_MAX];   // matrix column each key sits in
static uint8_t    serialNeedCols = 0;             // columns the console must read to see the current key
static TIKeyCombo serialKey;
static SerialPhase serialPhase = SERIAL_IDLE;
static int        serialFrames = 0;
static int        serialIdlePolls = 0;   // consecutive frames the editor was seen polling, after ENTER

static inline unsigned serialRingUsed(void) { return (serialHead - serialTail) & (SERIAL_RING_SIZE - 1); }
static inline unsigned serialRingFree(void) { return SERIAL_RING_SIZE - 1 - serialRingUsed(); }

// The console settles what character each key produces, and says so in its own GROM:
// three 48-byte tables at 0x1700 (plain), 0x1730 (SHIFT) and 0x1760 (FCTN), one entry per
// key. They are indexed by matrix position rather than by key - col*8 + (7-row) into
// TIKeys[row][col] - so reading them backwards gives the character -> keypress mapping the
// machine itself will honour, which beats maintaining a second copy of it here. Between
// them the three tables cover every printable ASCII character; filling plain first, then
// SHIFT, then FCTN resolves each character to the least modifier that produces it.
static void serialKeyboardBegin(void)
{
    memset(serialAsciiToTI, 0, sizeof(serialAsciiToTI));
    serialLutReady = false;
    serialPhase    = SERIAL_IDLE;
    serialFrames   = 0;
    serialHead = serialTail = 0;
    serialPrevByte       = 0;
    serialCancelReq      = false;
    serialOverflowed     = false;
    serialOverflowWarned = false;
    // A cartridge change or reset is a fresh start: swallow anything the sender still has
    // in flight from before rather than typing it at the new machine.
    serialDiscardUntil   = time_us_32() + SERIAL_DISCARD_QUIET_US;
    serialDiscarding     = true;
    serialRxCount        = 0;
    serialTypedCount     = 0;
    serialXoffUsed       = false;

    if (!MemGROM) return;

    // Entries 5 and 6 of the plain table are ENTER and SPACE in every console GROM. If
    // they are not there this is not a translation table - a cartridge that replaces
    // console GROM can land here - so stay inert rather than type nonsense at the machine.
    if (MemGROM[0x1705] != 0x0d || MemGROM[0x1706] != 0x20)
    {
        printf("Serial keyboard: no key tables in GROM, disabled\n");
        return;
    }

    // Which column each key is wired to, so a synthesised press can tell when the console
    // has looked at it. Modifiers all live in column 0.
    memset(serialKeyColumn, 0, sizeof(serialKeyColumn));
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            serialKeyColumn[TIKeys[row][col]] = (uint8_t)col;

    static const struct { u16 base; u8 modifier; } tables[] = {
        {0x1700, TMS_KEY_NONE}, {0x1730, TMS_KEY_SHIFT}, {0x1760, TMS_KEY_FUNCTION},
    };

    for (const auto &t : tables)
    {
        for (int i = 0; i < 48; i++)
        {
            u8 key = TIKeys[7 - (i % 8)][i / 8];
            u8 ch  = MemGROM[t.base + i];
            if (key == TMS_KEY_NONE || key >= TMS_KEY_JOY1_UP) continue;  // dead cell or a joystick column
            if (ch < 0x20 || ch > 0x7e) continue;                         // FCTN's edit keys, not characters
            if (serialAsciiToTI[ch].key != TMS_KEY_NONE) continue;        // a plainer form already claimed it
            serialAsciiToTI[ch] = {key, t.modifier};
        }
    }
    serialLutReady = true;
}

static TIKeyCombo serialCharToTIKey(uint8_t c)
{
    switch (c)
    {
        case '\r':
        case '\n': return {TMS_KEY_ENTER, TMS_KEY_NONE};
        case '\t': return {TMS_KEY_SPACE, TMS_KEY_NONE};   // the TI has no tab
        case 0x08:
        case 0x7f: return {TMS_KEY_S,     TMS_KEY_FUNCTION};   // FCTN+S is the TI's backspace
        default:   return (c < 0x80) ? serialAsciiToTI[c] : TIKeyCombo{TMS_KEY_NONE, TMS_KEY_NONE};
    }
}

// Drop whatever is queued and let the sender go again.
static void serialKeyboardCancel(void)
{
    serialTail       = serialHead;
    serialPhase      = SERIAL_IDLE;
    serialFrames     = 0;
    serialPrevByte       = 0;
    serialCancelReq      = false;
    serialOverflowed     = false;
    serialOverflowWarned = false;

    // Let the sender go, then throw away whatever it had queued behind the XOFF - that
    // backlog is the rest of the abandoned paste, not the start of the next one.
    serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
    serialDiscarding   = true;
    serialRxCount      = 0;
    serialTypedCount   = 0;
    serialXoffUsed     = false;
    if (serialXoffSent) { putchar_raw(ASCII_XON); serialXoffSent = false; }
}

// Draining the UART has to be interrupt-driven, not polled. The emulation loop is not
// free-running: hstx_paceFrame() busy-waits for vsync, and because the TMS9900 is slow the
// emulator reaches that wait early and sits in it for much of every frame. Nothing polled
// from the main loop runs during that window, and the UART's 32-byte FIFO overruns after
// 2.8ms at 115200 baud - which is how a paste used to lose contiguous runs of characters
// while the ring buffer never even filled.
#ifdef uart_default
static void __not_in_flash_func(serialUartIrq)(void)
{
    while (uart_is_readable(uart_default))
    {
        uint8_t c = (uint8_t)uart_get_hw(uart_default)->dr;   // reading DR also clears RX/RT

        if (!settings.flags.serialKeyboard) continue;         // switched off: drain and discard
        if (c == ASCII_XON || c == ASCII_XOFF) continue;      // flow control aimed at us, not data
        if (c == ASCII_ETX) { serialCancelReq = true; continue; }

        if (serialDiscarding)
        {
            // Still emptying the sender's backlog. Every byte pushes the quiet period out
            // again, so this ends only once the line has genuinely gone silent.
            serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
            continue;
        }

        // CRLF is one ENTER, not two: swallow the LF that follows a CR. A lone CR and a
        // lone LF still end the line, so all three line endings behave alike.
        bool isLfAfterCr = (c == '\n' && serialPrevByte == '\r');
        serialPrevByte = c;
        if (isLfAfterCr) continue;

        unsigned next = (serialHead + 1) & (SERIAL_RING_SIZE - 1);
        if (next == serialTail) { serialOverflowed = true; continue; }   // full: drop
        serialRing[serialHead] = c;
        serialHead = next;
        serialRxCount++;
        serialLastRxUs = time_us_32();
    }
}

static void serialKeyboardIrqInit(void)
{
    // stdio_uart only claims the UART interrupt if a chars-available callback is
    // registered, and nothing here registers one, so the vector is ours to take. TX is
    // left alone: printf keeps going through stdio as before.
    uint irqNum = UART_IRQ_NUM(uart_default);
    irq_set_exclusive_handler(irqNum, serialUartIrq);
    irq_set_enabled(irqNum, true);
    uart_set_irqs_enabled(uart_default, true, false);
}
#else
static void serialKeyboardIrqInit(void) { }   // no default UART instance to listen on
#endif

// Flow control and housekeeping. Once a frame is plenty now that the interrupt does the
// draining: the ring holds 4KB and XOFF goes out at half full, so there is ~2KB of slack
// against a worst case 16.7ms of pacing latency (~192 bytes at 115200 baud).
static void serialKeyboardPump(void)
{
    if (!settings.flags.serialKeyboard)
    {
        // Switched off mid-paste: drop what is queued so turning it back on does not
        // resume a listing the user has finished with.
        if (serialRingUsed() || serialPhase != SERIAL_IDLE) serialKeyboardCancel();
        return;
    }

    // The sender's backlog has stopped arriving, so anything from here on is new.
    if (serialDiscarding && (int32_t)(time_us_32() - serialDiscardUntil) > 0)
        serialDiscarding = false;

    if (serialCancelReq)
    {
        serialCancelReq = false;
        serialKeyboardCancel();
    }

    if (serialOverflowed && !serialOverflowWarned)
    {
        // Say so once. Silently dropping input is what turns a paste into a corrupted
        // listing, and the message lands in the terminal the paste came from.
        serialOverflowWarned = true;
        // No echo-swallowing here, unlike the end-of-paste summary: this one is printed
        // while the paste is still arriving, and discarding half a second of it to dodge
        // an echo would destroy far more than the echo ever could.
        printf("\r\nSerial keyboard: input overflow - enable XON/XOFF flow control\r\n");
    }

    // A paste is over once everything queued has been typed and nothing new has arrived
    // for a second. Report the total then: set against what was sent, it separates "the
    // board never received it" from "the board mistyped it". The 100 character floor
    // keeps the report to actual pastes, but the counters are cleared either way, so a
    // few stray characters cannot quietly add themselves to the next paste's total.
    if (serialRxCount && !serialRingUsed() && serialPhase == SERIAL_IDLE &&
        (int32_t)(time_us_32() - serialLastRxUs) > 1000000)
    {
        if (serialRxCount >= 100)
        {
            printf("\r\nSerial keyboard: %u characters received, %u typed%s\r\n",
                   (unsigned)serialRxCount, (unsigned)serialTypedCount,
                   serialXoffUsed ? " (flow control was used)" : "");
            // This port is also the keyboard, so anything echoed back - by a host tty with
            // ECHO left on, or a terminal in local echo - would be typed into the machine
            // as though it had been keyed in. Swallow our own words.
            serialDiscardUntil = time_us_32() + SERIAL_DISCARD_QUIET_US;
            serialDiscarding   = true;
        }
        serialRxCount    = 0;
        serialTypedCount = 0;
        serialXoffUsed   = false;
    }

    // Hold the sender off well before the ring fills, release it once the machine has
    // caught up. The gap between the two thresholds is what stops XON/XOFF chattering.
    if (!serialXoffSent && serialRingUsed() > SERIAL_XOFF_USED)
    {
        putchar_raw(ASCII_XOFF);
        serialXoffSent = true;
        serialXoffUsed = true;
    }
    else if (serialXoffSent && serialRingUsed() < SERIAL_XON_USED)
    {
        putchar_raw(ASCII_XON);
        serialXoffSent = false;
    }
}

// Runs once a frame, straight after update_ti_keyboard() has cleared the matrix, so what
// is asserted here is what the console scans for the whole of the frame that follows.
static void serialKeyboardTick(void)
{
    if (!settings.flags.serialKeyboard || !serialLutReady) return;

    // A hand on the real keyboard wins: abandon the paste rather than fight it for the
    // matrix. This is also the way out if a paste goes wrong and no terminal is to hand.
    const auto &kb = io::getCurrentKeyboardState();
    if (kb.keycode[0] || kb.modifier)
    {
        if (serialPhase != SERIAL_IDLE || serialRingUsed()) serialKeyboardCancel();
        return;
    }

    if (serialPhase == SERIAL_IDLE)
    {
        TIKeyCombo k{TMS_KEY_NONE, TMS_KEY_NONE};
        while (serialRingUsed())
        {
            uint8_t c = serialRing[serialTail];
            serialTail = (serialTail + 1) & (SERIAL_RING_SIZE - 1);
            k = serialCharToTIKey(c);
            if (k.key != TMS_KEY_NONE) break;   // skip anything the TI has no key for
        }
        if (k.key == TMS_KEY_NONE) return;      // ring empty, or held nothing typeable

        serialKey       = k;
        serialTypedCount++;
        serialNeedCols  = (uint8_t)(1u << serialKeyColumn[k.key]);
        if (k.modifier != TMS_KEY_NONE) serialNeedCols |= (uint8_t)(1u << serialKeyColumn[k.modifier]);
        serialPhase     = SERIAL_HOLD;
        serialFrames    = 0;
        tms9901.KeyColsScanned = 0;   // only reads from here on count as having seen this key
    }

    // Case comes from SHIFT alone - the plain GROM table is lowercase and the SHIFT table
    // uppercase - so Alpha Lock must not get a vote, or a pasted lowercase listing would
    // arrive in capitals whenever the user happens to have it latched down.
    tms9901.CapsLock = 0;

    // The console has read every column this key needs, so it has seen the press - or,
    // in the gap, seen the key back up again.
    bool seen = (tms9901.KeyColsScanned & serialNeedCols) == serialNeedCols;
    serialFrames++;

    if (serialPhase == SERIAL_HOLD)
    {
        if ((seen && serialFrames >= MIN_HOLD_FRAMES) || serialFrames >= SCAN_TIMEOUT_FRAMES)
        {
            // Seen. Release it: the console debounces on release, so it will not accept
            // the same key twice without one sweep showing the key up in between.
            serialPhase            = SERIAL_GAP;
            serialFrames           = 0;
            serialIdlePolls        = 0;
            tms9901.KeyColsScanned = 0;
            tms9901.KeySweeps      = 0;   // count from the first frame with the key up
            return;
        }
        tms9901.Keyboard[serialKey.key] = 1;
        if (serialKey.modifier != TMS_KEY_NONE) tms9901.Keyboard[serialKey.modifier] = 1;
    }
    else
    {
        // KeySweeps holds exactly the frame just run: cleared at the release above, and
        // again here on every tick of the gap.
        serialIdlePolls   = (tms9901.KeySweeps >= EDITOR_POLL_SWEEPS) ? serialIdlePolls + 1 : 0;
        tms9901.KeySweeps = 0;

        // After ENTER only the editor polling at its own rate shows it is listening again
        // (see EDITOR_POLL_SWEEPS). Any other key just needs its columns seen with it up.
        bool done = (serialKey.key == TMS_KEY_ENTER)
                  ? (serialIdlePolls >= EDITOR_POLL_FRAMES || serialFrames >= ENTER_TIMEOUT_FRAMES)
                  : ((seen && serialFrames >= MIN_GAP_FRAMES) || serialFrames >= SCAN_TIMEOUT_FRAMES);
        if (done)
        {
            serialPhase  = SERIAL_IDLE;
            serialFrames = 0;
        }
    }
}

#else   // !SERIAL_KEYBOARD_AVAILABLE

static inline void serialKeyboardBegin(void) { }
static inline void serialKeyboardPump(void)  { }
static inline void serialKeyboardTick(void)  { }

#endif

// -------------------------------------------------------------------------------------
// Joysticks. The TI's two joystick ports are scanned as extra keyboard columns, so they
// land in the same tms9901.Keyboard[] array. USB gamepads take a port each; a GPIO
// NES/SNES pad or a Wii classic controller feeds whichever port has no USB pad.
// -------------------------------------------------------------------------------------
static void update_ti_joysticks(void)
{
    static constexpr int LEFT   = 1 << 6;
    static constexpr int RIGHT  = 1 << 7;
    static constexpr int UP     = 1 << 4;
    static constexpr int DOWN   = 1 << 5;
    static constexpr int SELECT = 1 << 2;
    static constexpr int START  = 1 << 3;
    static constexpr int A      = 1 << 0;
    static constexpr int B      = 1 << 1;

#if NES_PIN_CLK != -1
    // A NES pad shifts its buttons out in NES order already; a SNES pad puts B and Y
    // where A and B would be, so its face buttons are taken by name rather than by
    // position - otherwise physical A does nothing and B fires.
    auto nespadGameBits = [](int padnum) -> int
    {
        if (nespad_padtype[padnum] != NESPAD_TYPE_SNES)
        {
            return nespad_states[padnum];
        }
        const uint16_t ext = nespad_states_ext[padnum];
        int v = ext & (SELECT | START | UP | DOWN | LEFT | RIGHT);
        if (ext & (1u << 8)) v |= A;
        if (ext & (1u << 0)) v |= B;
        return v;
    };
#endif

    static const u8 joyKeys[2][5] = {
        { TMS_KEY_JOY1_UP, TMS_KEY_JOY1_DOWN, TMS_KEY_JOY1_LEFT, TMS_KEY_JOY1_RIGHT, TMS_KEY_JOY1_FIRE },
        { TMS_KEY_JOY2_UP, TMS_KEY_JOY2_DOWN, TMS_KEY_JOY2_LEFT, TMS_KEY_JOY2_RIGHT, TMS_KEY_JOY2_FIRE },
    };

    bool usbConnected = false;

    for (int i = 0; i < 2; ++i)
    {
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0) usbConnected = gp.isConnected();

        int v = (gp.buttons & io::GamePadState::Button::LEFT   ? LEFT   : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT  ? RIGHT  : 0) |
                (gp.buttons & io::GamePadState::Button::UP     ? UP     : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN   ? DOWN   : 0) |
                (gp.buttons & io::GamePadState::Button::A      ? A      : 0) |
                (gp.buttons & io::GamePadState::Button::B      ? B      : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START  ? START  : 0);

#if NES_PIN_CLK != -1
        if (usbConnected)
        {
            if (i == 1) v |= nespadGameBits(1) | nespadGameBits(0);
        }
        else
        {
            v |= nespadGameBits(i);
        }
#endif

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected) { if (i == 1) v |= wiipad_raw_cached; }
        else              { if (i == 0) v |= wiipad_raw_cached; }
#endif

        if (v & UP)            tms9901.Keyboard[joyKeys[i][0]] = 1;
        if (v & DOWN)          tms9901.Keyboard[joyKeys[i][1]] = 1;
        if (v & LEFT)          tms9901.Keyboard[joyKeys[i][2]] = 1;
        if (v & RIGHT)         tms9901.Keyboard[joyKeys[i][3]] = 1;
        if (v & (A | B))       tms9901.Keyboard[joyKeys[i][4]] = 1;

        if (i == 0)
        {
            // -------------------------------------------------------------------------
            // Nearly every TI cartridge comes up on a master title screen asking for a
            // number - "1 FOR TI BASIC", "2 FOR PARSEC" and so on - so a gamepad alone
            // cannot get into a game. Put those two keys on SELECT and START.
            //
            // Both buttons are also modifiers for the emulator's own shortcuts, so only
            // send the keypress when the button is on its own: SELECT pairs with START
            // and the d-pad, START pairs with SELECT, A and left/right. Without this
            // guard, opening the settings menu would type a 1 into the running game.
            // -------------------------------------------------------------------------
            if ((v & SELECT) && !(v & (START | UP | DOWN | LEFT | RIGHT)))
                tms9901.Keyboard[TMS_KEY_1] = 1;

            if ((v & START) && !(v & (SELECT | A | LEFT | RIGHT)))
                tms9901.Keyboard[TMS_KEY_2] = 1;

            // Reboot to BOOTSEL mode
            if ((v & (SELECT | START | UP | A)) == (SELECT | START | UP | A))
                reset_usb_boot(0, 0);

            auto pushed = v & ~prevButtons[i];

            if (v & START)
            {
                if (pushed & A)
                {
                    settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
                }
#if HW_CONFIG == 8
                else if (pushed & LEFT)
                {
                    settings.fruitjamVolumeLevel = std::max((int8_t)-63, (int8_t)(settings.fruitjamVolumeLevel - 1));
                    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
                }
                else if (pushed & RIGHT)
                {
                    settings.fruitjamVolumeLevel = std::min((int8_t)23, (int8_t)(settings.fruitjamVolumeLevel + 1));
                    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
                }
#endif
            }

            if (v & SELECT)
            {
                if (pushed & START) showSettings = true;
                if (pushed & UP)        scaleMode8_7_ = Frens::screenMode(-1);
                else if (pushed & DOWN) scaleMode8_7_ = Frens::screenMode(+1);
                else if (pushed & LEFT)
                {
#if EXT_AUDIO_IS_ENABLED && !HSTX
                    settings.flags.useExtAudio = !settings.flags.useExtAudio;
#else
                    settings.flags.useExtAudio = 0;
#endif
                }
#if ENABLE_VU_METER
                else if (pushed & RIGHT)
                {
                    settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                    turnOffAllLeds();
                }
#endif
            }
        }
        prevButtons[i] = v;
    }
}

// Forward declaration - the settings menu's Reset action needs this, and its definition
// sits with the rest of the TI BASIC handling below.
static bool is_tibasic_selection(const char *path);

// =====================================================================================
// Per-frame housekeeping
// =====================================================================================
static void processPerFrame(void)
{
    // Pace to vsync before the next frame is emulated, not after. Scanlines are written
    // straight into the framebuffer as the VDP produces them, so the emulator has to
    // start its frame at the top of vblank - that keeps the writer ahead of the scanout
    // DMA for the whole frame and there is nothing to tear.
    Frens::PaceFrames60fps(false);

    Frens::pollHeadPhoneJack();
    EXT_AUDIO_POLL_HEADPHONE();

#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    Frens::blinkLed((count / 60) & 1);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_raw_cached = wiipad_read();
#endif

#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
        turnOffAllLeds();
    }
#endif

    update_ti_keyboard();
    update_ti_joysticks();
    serialKeyboardPump();
    serialKeyboardTick();   // must follow update_ti_keyboard: it clears the whole matrix

    if (showSettings)
    {
        showSettings = false;
        int rval = showSettingsMenu(true);
        if (rval == 3)          // Quit game
        {
            key_done = true;
        }
        if (rval == 5)          // Reset game
        {
            // Reset must go back through the same path the game was started with, or a
            // TI BASIC session would try to load its own marker file as a cartridge.
            char err[64] = {0};
            const char *p = is_tibasic_selection(romName) ? nullptr : romName;
            ti99_load_cart(p, 0, err, sizeof(err));
            // Reloading the cart can replace console GROM, and half-typed text from
            // before the reset is no longer wanted anyway.
            serialKeyboardBegin();
        }
    }

    // The console has asked for a tape it has not got. SAVE CS1 and OLD CS1 name no
    // file, so this is the only moment the choice can be made - and it is the same
    // moment the console is telling the user to press RECORD or PLAY.
    int tapeReq = cassette_pending_request();
    if (tapeReq)
    {
        cassette_clear_request();
        menuCassettePrompt(tapeReq == TAPE_REQ_RECORD);
    }
}

// =====================================================================================
// TI BASIC marker file
//
// The menu is built around picking a file, but TI BASIC needs no cartridge at all - it
// lives in the console GROMs. A small marker file gives it a menu entry like any other
// title; selecting it boots the console with an empty cartridge slot.
// =====================================================================================
static void ensure_tibasic_marker(void)
{
    static const char *dirs[] = { "/roms/TI99", "/roms/ti99", "/roms" };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        FILINFO fno;
        if (f_stat(dirs[i], &fno) != FR_OK) continue;      // that rom folder does not exist

        char path[FF_MAX_LFN];
        snprintf(path, sizeof(path), "%s/%s", dirs[i], TIBASIC_MARKER);
        if (f_stat(path, &fno) == FR_OK) return;           // already there

        FIL fil;
        if (f_open(&fil, path, FA_WRITE | FA_CREATE_NEW) == FR_OK)
        {
            // Content is never read - selecting the file by its extension is the signal.
            const char *note = "pico-994A: boots the console with no cartridge (TI BASIC)\n";
            UINT bw;
            f_write(&fil, note, strlen(note), &bw);
            f_close(&fil);
            printf("[ti99] created %s\n", path);
        }
        return;
    }
}

static bool is_tibasic_selection(const char *path)
{
    if (!path || !path[0]) return true;
    const char *ext = strrchr(path, '.');
    return (ext && strcasecmp(ext, TIBASIC_EXT) == 0);
}

// =====================================================================================
// main
// =====================================================================================
int main()
{
    char selectedRom[FF_MAX_LFN];
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    Frens::setClocksAndStartStdio(CPUFreqKHz, VREG_VOLTAGE_1_20);

#if SERIAL_KEYBOARD_AVAILABLE
    // stdio only ever transmitted, so nothing cared what RX did. The serial keyboard
    // reads it, and a disconnected input left floating would frame garbage into the
    // machine - pull it to the idle-high a real sender would hold it at.
    gpio_pull_up(PICO_DEFAULT_UART_RX_PIN);
    serialKeyboardIrqInit();
#endif

    printf("==========================================================================================\n");
    printf("pico-994A (TI-99/4A) %s\n", SWVERSION);
    printf("Build date: %s  time: %s\n", __DATE__, __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d\n", clock_get_hz(clk_hstx) / 1000);
#endif
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");

    FrensSettings::initSettings(FrensSettings::TI99);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz,
                                   TI99_MARGIN_TOP, TI99_MARGIN_BOTTOM,
                                   AUDIOBUFFERSIZE, false, true);

    g_settings_visibility    = g_settings_visibility_ti99;
    g_available_screen_modes = g_available_screen_modes_ti99;
    menuSetCassetteHooks(&cassetteHooks);
    if (!g_available_screen_modes[static_cast<int>(settings.screenMode)])
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);

    // Default machine: NTSC, 32K expansion, plain cartridge, no frame skipping.
    // These fields are *indices* into tables in the core, not values - maxSprites 0
    // selects MaxSprites[0] == 4 (what a real 9918A shows on a line) and spriteCheck 0
    // means "use the default collision scan rate", not "check every 255 lines".
    memset(&myConfig, 0x00, sizeof(myConfig));
    myConfig.machineType = MACH_TYPE_NORMAL32K;
    myConfig.cartType    = CART_TYPE_NORMAL;
    myConfig.maxSprites  = 0;          // 0 = hardware accurate (4/line), 1 = 32, no flicker
    myConfig.spriteCheck = 0;          // 0 = default scan rate
    myConfig.frameSkip   = 0;          // 0 = never skip
    myConfig.isPAL       = 0;

    ensure_tibasic_marker();

    bool showSplash = true;

    while (true)
    {
        if (strlen(selectedRom) == 0)
        {
            // .rpk is the format to prefer - one file holding every part. The classic
            // C/D/G sets are .bin, and .tib is the marker that boots TI BASIC.
            // RomLister splits this list on spaces (see RomLister::IsextensionAllowed),
            // not commas, and compares case-insensitively.
            const char *romExtensions = ".rpk .bin .tib";
            menu("pico-994A", ErrorMessage, isFatalError, showSplash, romExtensions, selectedRom);
            printf("Selected: %s\n", selectedRom);
        }

        *ErrorMessage = 0;
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

        ti99_free_memory();
        if (ti99_alloc_memory() != 0)
        {
            strcpy(ErrorMessage, "Out of memory");
            printf("%s\n", ErrorMessage);
            selectedRom[0] = 0;
            continue;
        }
        Frens::dumpHeapStats("after ti99_alloc_memory");

        if (ti99_load_bios(ErrorMessage, 40) != 0)
        {
            printf("%s\n", ErrorMessage);
            ti99_free_memory();
            selectedRom[0] = 0;
            continue;
        }

        // 994aDISK.bin is loaded into PSRAM, so a board without it has no disk controller
        // to offer. Leaving the hooks null is what makes the menu show the row as N/A.
        menuSetDiskHooks(ti99_disk_dsr_available() ? &diskHooks : nullptr);

        const char *cartPath = is_tibasic_selection(selectedRom) ? nullptr : selectedRom;
        if (ti99_load_cart(cartPath, 1, ErrorMessage, 40) != 0)
        {
            printf("%s\n", ErrorMessage);
            ti99_free_memory();
            selectedRom[0] = 0;
            continue;
        }
        Frens::dumpHeapStats("after cart load");

        build_palette();
        ti99_psg_init(TI99_AUDIO_SAMPLE_RATE);
        // After the cart: loading one can replace console GROM, and that is where the
        // key translation tables the serial keyboard reads live.
        serialKeyboardBegin();

        if (showSplash && !Frens::isPsramEnabled())
        {
            showSplash = false;
            menuPumpBlankFrames(180);
        }

        Frens::PaceFrames60fps(true);
        start_tick_us = Frens::time_us();
        prevButtons[0] = prevButtons[1] = 0;
        key_done = false;

        // Emulation loop: run scanlines until the VDP says the frame is done, then
        // hand off audio and let processPerFrame pace us to the display.
        while (!key_done)
        {
            processPerFrame();                  // pace to vsync, then run the frame
            while (LoopTMS9900()) { }           // one frame of CPU + VDP, rendering as it goes
            cassette_frame_tick();              // tape prefetch / flush - the only SD access
            process_audio_frame();
            SpeechTraceTick();                  // no-op unless -DTI99_SPEECH_TRACE
        }

        // Back to the menu: release the machine so RomLister and the artwork loader
        // have the heap to themselves again.
        ti99_free_memory();
        Frens::dumpHeapStats("after ti99_free_memory");

        selectedRom[0] = 0;
        showSplash = false;
    }

    return 0;
}
