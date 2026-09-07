// =====================================================================================
// ti99_pico.c - port glue between the DS994a emulation core and pico-994A.
//
// Replaces upstream's DS99.c / DS99mngt.c / DS99_utils.c, which mixed the machine
// setup in with the Nintendo DS user interface. What is left here is only:
//
//   * ownership of the emulated machine's memory (allocated per game, freed on exit)
//   * loading the console ROM/GROM and the disk DSR from /bios/
//   * loading cartridges - .rpk, and the classic C/D/G/8/9/0 multi-file sets
//   * machine reset and the one-scanline emulation step
//
// Video, audio, input and the menu all live in main.cpp on top of pico_shared.
//
// See DS994a-README.md for the upstream copyright notice.
// =====================================================================================
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>

#include "ti99_compat.h"
#include "ti99_fileio.h"
#include "ti99.h"
#include "ti99_pico.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cpu/tms9918a/tms9918a.h"
#include "cpu/sn76496/sn76496_shim.h"
#include "SAMS.h"
#include "disk.h"
#include "pcode.h"
#include "cassette.h"
#include "rpk/rpk.h"
#include "speech.h"

// -------------------------------------------------------------------------------------
// Globals the core reaches for (declared in ti99.h)
// -------------------------------------------------------------------------------------
struct Config_t myConfig;

u8  *SharedMemBuffer    = NULL;
u8  *SharedMemBufferBig = NULL;
u8  *DISK_DSR           = NULL;
u8   fileBuf[0x2000];
char tmpBuf[MAX_PATH];
u32  file_size          = 0;

// The console ROM and GROMs are read straight into MemCPU[0] / MemGROM[0] rather than
// kept in a second copy. Holding 32K of cache to avoid an SD read on the rare reset is
// a poor trade here - SRAM is the binding constraint and reset is user-initiated.
static char biosPathROM[MAX_PATH];    // remembered so reset can re-read without searching
static char biosPathGROM[MAX_PATH];

static u8  biosLoaded  = 0;
static u8  diskDsrLoaded = 0;

// Vocabulary ROM for the Speech Synthesizer. Optional: without it Speak External still
// works, which is how cartridges do speech; only the resident vocabulary that TI
// Extended BASIC's CALL SAY reaches needs this file.
static u8 *speechROM = NULL;

// -------------------------------------------------------------------------------------
// Small stubs for DS-only entry points the core still calls.
// -------------------------------------------------------------------------------------
void DS_Print(int x, int y, int scr, char *msg)
{
    (void)x; (void)y; (void)scr;
    if (msg && msg[0] && msg[0] != ' ') printf("[ti99] %s\n", msg);
}

// Upstream nudges the DS button mapping for two titles whose RPK names it recognises.
// This port maps a real gamepad and keyboard directly, so there is nothing to adjust.
void SetDiagonals(void) { }
void MapPlayer2(void)   { }

// -------------------------------------------------------------------------------------
// Memory ownership
// -------------------------------------------------------------------------------------
static void free_ptr(u8 **p) { if (*p) { free(*p); *p = NULL; } }

void ti99_free_memory(void)
{
    disk_init();                 // closes any open .DSK handles
    cassette_shutdown();         // closes the tape and frees its buffers
    SAMS_Release();              // returns the SAMS backing store to PSRAM

    // MemCPU and MemGROM came from ti99_mem_alloc, so they go back through its free.
    if (MemCPU)  { ti99_mem_free(MemCPU);  MemCPU  = NULL; }
    if (MemGROM) { ti99_mem_free(MemGROM); MemGROM = NULL; }
    free_ptr(&pVDPVidMem);
    // SharedMemBuffer / SharedMemBufferBig are upstream's DS scratch buffers. Nothing in
    // this port allocates them - DSK3 is not buffered and SAMS has its own allocation -
    // so they stay NULL and there is nothing to release.

    // These came from ti99_psram_alloc, so they go back the same way.
    if (DISK_DSR)  { ti99_psram_free(DISK_DSR);  DISK_DSR  = NULL; }
    SpeechSetROM(NULL, 0);
    if (speechROM) { ti99_psram_free(speechROM); speechROM = NULL; }

    if (MemCART) { ti99_cart_free(); }
    biosLoaded = 0;
    diskDsrLoaded = 0;
}

// Returns 0 on success. Everything here is per-game and released by ti99_free_memory().
int ti99_alloc_memory(void)
{
    // The two 64K address spaces are the largest thing the machine needs, so they go to
    // PSRAM on boards that have it and free 128K of SRAM. ti99_mem_alloc falls back to
    // SRAM where there is no PSRAM, so nothing is excluded - the emulator just runs in
    // the tighter footprint it used to.
    //
    // Video RAM stays in SRAM deliberately: the VDP walks it for every scanline it
    // renders, and 16K is cheap enough not to be worth the latency.
    MemCPU      = (u8 *)ti99_mem_alloc(0x10000);   // 64K CPU address space
    MemGROM     = (u8 *)ti99_mem_alloc(0x10000);   // 64K GROM address space
    pVDPVidMem  = (u8 *)malloc(0x4000);            // 16K VDP video RAM

    // The SDK's malloc panics instead of returning NULL, so in practice a shortfall
    // shows up as a board reset rather than as an error here. These checks are kept for
    // correctness, but the thing that actually keeps this working is the budget: 144K of
    // SRAM for the machine, leaving room for the menu, FatFS, USB and the cartridge.
    if (!MemCPU || !MemGROM || !pVDPVidMem)
    {
        ti99_free_memory();
        return -1;
    }

    memset(MemCPU,     0x00, 0x10000);
    memset(MemGROM,    0x00, 0x10000);
    memset(pVDPVidMem, 0x00, 0x4000);

    return 0;
}

// -------------------------------------------------------------------------------------
// How much SRAM heap is still available.
//
// mallinfo() only describes the arena malloc has already taken from sbrk, so its
// fordblks alone reads as almost nothing on a board that has not allocated much yet -
// the rest of the heap is simply not claimed. The part sbrk can still hand out has to
// be added to it: the heap runs from `end` (first byte above .bss) up to __StackLimit,
// which is where the SDK's _sbrk stops.
//
// This exists because the SDK's malloc **panics** rather than returning NULL. Anything
// that might not fit has to be checked before it is asked for, not after.
// -------------------------------------------------------------------------------------
u32 ti99_sram_free(void)
{
    extern char end;            // first byte of the heap  (newlib linker symbol)
    extern char __StackLimit;   // heap ceiling            (pico-sdk _sbrk)

    struct mallinfo mi = mallinfo();

    u32 capacity = (u32)(&__StackLimit - &end);
    u32 claimed  = (u32)mi.arena;               // already taken from sbrk
    u32 unclaimed = (capacity > claimed) ? (capacity - claimed) : 0;

    return unclaimed + (u32)mi.fordblks;        // never handed out + free in the arena
}

// -------------------------------------------------------------------------------------
// Cartridge memory. Sized to the cart rather than upstream's fixed 512K/8MB, and kept
// in SRAM while it fits - tms9900.cartBankPtr points straight into it and every fetch
// from >6000 reads through it, so it is hot. Oversized carts go to PSRAM if fitted.
// -------------------------------------------------------------------------------------
// Above this a cart goes to PSRAM instead. Sized so a large cart cannot squeeze out
// the ~200K the rest of the machine needs; the great majority of TI carts are 8-32K.
#define CART_SRAM_LIMIT   (64 * 1024)

// What has to stay free after the cart is in. On a board with no PSRAM the two 64K
// address spaces and the 16K of video RAM come out of the same heap, so this is what
// separates "tight" from "the next allocation kills the board": the settings menu's
// screen buffer (~4K), the tape and disk listers (~4K), the tape I/O buffer (4K), FatFS
// handles and stdio buffers, plus slack.
#define CART_SRAM_RESERVE (24 * 1024)

// The SDK's malloc panics rather than returning NULL when it cannot satisfy a request,
// so "try SRAM, fall back to PSRAM" is not a thing that can work - the fallback is
// unreachable and the board dies instead. Every allocation decision here is therefore
// made from the size up front.
static u8 cartInPsram = 0;

void ti99_cart_free(void)
{
    if (MemSuperCartRAM) { ti99_psram_free(MemSuperCartRAM); MemSuperCartRAM = NULL; }

    if (MemCART)
    {
        if (cartInPsram) ti99_psram_free(MemCART);
        else             free(MemCART);
        MemCART = NULL;
    }
    cartInPsram   = 0;
    MAX_CART_SIZE = 0;
}

int ti99_cart_alloc(u32 size)
{
    ti99_cart_free();

    // Round up to a power-of-two count of 8K banks. This is not tidiness: WriteBank()
    // masks the requested bank with tms9900.bankMask, which BankMasks[] rounds up to
    // the next power of two, so a five-bank cart can legally be asked for bank seven.
    // Upstream never notices because the DS allocates a fixed 512K/8MB; sizing the
    // buffer to the cart means the allocation has to cover the whole masked range or
    // that bank switch reads past the end of the heap block.
    if (size < 0x2000) size = 0x2000;            // always at least one 8K bank
    u32 banks = (size + 0x1FFF) / 0x2000;
    u32 pow2  = 1;
    while (pow2 < banks) pow2 <<= 1;
    size = pow2 * 0x2000;

    // Inside the budget *and* actually there. The limit on its own is not enough on a
    // board with no PSRAM: MemCPU, MemGROM and the video RAM are 144K of the same heap,
    // so what is left over is around 110K, and the framework holds some of that. Asking
    // for a cart that does not fit panics instead of failing, so the heap is measured
    // first and the request only made when it can be met.
    u32 freeSram = ti99_sram_free();
    if (size <= CART_SRAM_LIMIT && (size + CART_SRAM_RESERVE) <= freeSram)
    {
        MemCART = (u8 *)malloc(size);
        cartInPsram = 0;
    }
    else
    {
        // Too big for the SRAM budget, or there is not enough of it left: this needs
        // PSRAM, and ti99_psram_alloc answers NULL rather than panicking without it.
        MemCART = (u8 *)ti99_psram_alloc(size);
        cartInPsram = (MemCART != NULL);
        if (!MemCART)
            printf("[ti99] cart needs %uK, only %uK of SRAM free and no PSRAM\n",
                   (unsigned)(size >> 10), (unsigned)(freeSram >> 10));
    }
    if (!MemCART) return -1;

    memset(MemCART, 0xFF, size);
    MAX_CART_SIZE = size;
    return 0;
}

// -------------------------------------------------------------------------------------
// BIOS
//
// /bios/994aROM.bin   8K  console ROM
// /bios/994aGROM.bin 24K  console GROMs - this is where TI BASIC lives
// /bios/994aDISK.bin  8K  TI disk controller DSR (optional)
//
// The first two are required; without them there is no machine to emulate. Upstream
// looks in /roms/bios and /roms/ti99, which are checked too so an SD card prepared for
// DS994a works unchanged.
// -------------------------------------------------------------------------------------
static const char *bios_dirs[] = { "/bios", "/roms/bios", "/roms/ti99", "" };

static int load_bios_file(const char *name, u8 *dest, u32 expect, u32 *got, char *foundPath)
{
    for (int i = 0; i < (int)(sizeof(bios_dirs)/sizeof(bios_dirs[0])); i++)
    {
        snprintf(tmpBuf, MAX_PATH, "%s/%s", bios_dirs[i], name);
        FILE *f = fopen(tmpBuf, "rb");
        if (!f) continue;

        size_t n = fread(dest, 1, expect, f);
        fclose(f);
        if (got) *got = (u32)n;
        if (foundPath) { strncpy(foundPath, tmpBuf, MAX_PATH - 1); foundPath[MAX_PATH - 1] = 0; }
        printf("[ti99] loaded %s (%u bytes)\n", tmpBuf, (unsigned)n);
        return (n > 0) ? 0 : -1;
    }
    return -1;
}

// Re-read the console ROM and GROMs into the emulated address space. Called on every
// cart load and reset, from the paths remembered when the BIOS was first found.
static int reload_console_images(void)
{
    if (!biosPathROM[0] || !biosPathGROM[0]) return -1;

    FILE *f = fopen(biosPathROM, "rb");
    if (!f) return -1;
    size_t n = fread(&MemCPU[0], 1, 0x2000, f);
    fclose(f);
    if (n != 0x2000) return -1;

    f = fopen(biosPathGROM, "rb");
    if (!f) return -1;
    n = fread(&MemGROM[0], 1, 0x6000, f);
    fclose(f);
    return (n == 0x6000) ? 0 : -1;
}

int ti99_load_bios(char *errorMessage, size_t errorMessageSize)
{
    if (!MemCPU || !MemGROM) return -1;

    biosPathROM[0] = biosPathGROM[0] = 0;

    memset(&MemCPU[0],  0xFF, 0x2000);
    memset(&MemGROM[0], 0xFF, 0x6000);

    u32 romBytes = 0, gromBytes = 0;

    if (load_bios_file("994aROM.bin", &MemCPU[0], 0x2000, &romBytes, biosPathROM) != 0)
    {
        snprintf(errorMessage, errorMessageSize, "Missing /bios/994aROM.bin");
        return -1;
    }
    if (load_bios_file("994aGROM.bin", &MemGROM[0], 0x6000, &gromBytes, biosPathGROM) != 0)
    {
        snprintf(errorMessage, errorMessageSize, "Missing /bios/994aGROM.bin");
        return -1;
    }

    // A truncated console GROM boots to a blank or garbled title screen, which is a
    // confusing way to fail - say so plainly instead.
    if (romBytes < 0x2000 || gromBytes < 0x6000)
    {
        snprintf(errorMessage, errorMessageSize, "BIOS too small (ROM %u, GROM %u)",
                 (unsigned)romBytes, (unsigned)gromBytes);
        return -1;
    }

    biosLoaded = 1;

    // The disk DSR is optional - without it DSK1-3 simply do not answer. It lives in
    // PSRAM, so disk support needs a board that has some: the SRAM budget is already
    // spoken for by the 64K CPU space, 64K GROM space, 16K of video RAM and the
    // cartridge, and there is no room to take another 8K out of it.
    // ti99_psram_alloc returns NULL rather than panicking when there is no PSRAM.
    if (!DISK_DSR) DISK_DSR = (u8 *)ti99_psram_alloc(0x2000);
    if (DISK_DSR)
    {
        memset(DISK_DSR, 0xFF, 0x2000);
        diskDsrLoaded = (load_bios_file("994aDISK.bin", DISK_DSR, 0x2000, NULL, NULL) == 0);
        if (!diskDsrLoaded)
        {
            ti99_psram_free(DISK_DSR);
            DISK_DSR = NULL;
            printf("[ti99] no 994aDISK.bin - DSK1-3 will not be available\n");
        }
    }
    else
    {
        printf("[ti99] no PSRAM - DSK1-3 not available\n");
    }

    // The Speech Synthesizer is always reported as attached: nearly all speech in
    // cartridges is streamed with Speak External and needs no vocabulary ROM at all.
    // spchrom.bin only adds the resident vocabulary.
    // The 32K vocabulary ROM goes to PSRAM for the same reason as the disk DSR, and it
    // is four times the size. Without PSRAM the module still works - cartridge speech is
    // streamed with Speak External and needs no vocabulary ROM - only CALL SAY is lost.
    if (!speechROM) speechROM = (u8 *)ti99_psram_alloc(TI99_SPEECH_ROM_SIZE);
    if (speechROM)
    {
        memset(speechROM, 0x00, TI99_SPEECH_ROM_SIZE);
        u32 got = 0;
        if (load_bios_file("spchrom.bin", speechROM, TI99_SPEECH_ROM_SIZE, &got, NULL) == 0)
        {
            SpeechSetROM(speechROM, got);
        }
        else
        {
            ti99_psram_free(speechROM);
            speechROM = NULL;
            SpeechSetROM(NULL, 0);      // module present, no resident vocabulary
            printf("[ti99] no spchrom.bin - speech works, resident vocabulary does not\n");
        }
    }
    else
    {
        SpeechSetROM(NULL, 0);          // no PSRAM: cartridge speech only
        printf("[ti99] no PSRAM - spchrom.bin not loaded, CALL SAY not available\n");
    }
    SpeechSetChip(SPEECH_CHIP_TMS5200);   // what the TI-99/4A module actually shipped with

    return 0;
}

int ti99_disk_dsr_available(void) { return diskDsrLoaded; }

// -------------------------------------------------------------------------------------
// Machine reset. Upstream's ResetTI() minus the DS timers, touch keyboard and debugger.
// -------------------------------------------------------------------------------------
void ti99_reset(u8 initDisks)
{
    SpeechInit();
    SAMS_Initialize();
    Reset9918();

    ti99_psg_init(TI99_AUDIO_SAMPLE_RATE);
    sn76496W(0x90 | 0x0F, &snti99);     // channel A volume off
    sn76496W(0xB0 | 0x0F, &snti99);     // channel B volume off
    sn76496W(0xD0 | 0x0F, &snti99);     // channel C volume off
    sn76496W(0xF0 | 0x0F, &snti99);     // noise    volume off

    if (initDisks) disk_init();
    pcode_init();
    cassette_init();
}

// -------------------------------------------------------------------------------------
// One scanline of CPU, then one scanline of VDP. Returns 0 when a frame is complete.
// Identical in structure to upstream LoopTMS9900() in DS99mngt.c.
// -------------------------------------------------------------------------------------
u32 LoopTMS9900(void)
{
    if (tms9900.accurateEmuFlags) TMS9900_RunAccurate();
    else                          TMS9900_Run();

    if (Loop9918()) TMS9901_RaiseVDPInterrupt();

    return ((CurLine == tms_end_line) ? 0 : 1);
}

// =====================================================================================
// Cartridge loading
//
// Adapted from upstream TI99Init() in DS99mngt.c. Two shapes are supported:
//
//   .rpk        a MAME/MESS Rom PacK - a zip holding every part plus layout.xml.
//               One file, so this is the format to prefer on SD.
//   xxxC.bin    the classic multi-file set. The menu picks any one part and the rest
//               are opened alongside it:
//                 xxxC.bin  CPU ROM at >6000        xxxD.bin  second 8K bank
//                 xxxG.bin  GROM at >6000           xxx8.bin  multi-bank, non-inverted
//                 xxx9.bin  multi-bank, inverted    xxx0.bin  replaces the console GROMs
//
// A cart is optional: with none loaded the console boots its own GROMs and the master
// title screen offers TI BASIC, which is exactly what the .tib marker file selects.
// =====================================================================================

// Load one part file alongside the selected one, e.g. "GAMEC.bin" -> "GAMEG.bin".
// Returns bytes read, or 0 if that part does not exist.
static u32 load_part(char *path, char partChar, u8 *dest, u32 maxBytes)
{
    size_t len = strlen(path);
    if (len < 5) return 0;

    char saved = path[len - 5];
    path[len - 5] = partChar;

    u32 n = 0;
    FILE *f = fopen(path, "rb");
    if (f)
    {
        n = (u32)fread(dest, 1, maxBytes, f);
        fclose(f);
        printf("[ti99] cart part '%c': %u bytes\n", partChar, (unsigned)n);
    }

    path[len - 5] = saved;
    return n;
}

static void apply_cart_type(void)
{
    // Supercart: 32K of CRU-banked RAM in the cart slot.
    if (myConfig.cartType == CART_TYPE_SUPERCART)
    {
        for (u32 address = 0x6000; address < 0x8000; address++) MemType[address >> 4] = MF_RAM8;
        memset(MemCPU + 0x6000, 0x00, 0x2000);
    }

    // Mini Memory: 4K of battery-backed RAM in the upper half of the slot.
    if (myConfig.cartType == CART_TYPE_MINIMEM)
    {
        for (u32 address = 0x7000; address < 0x8000; address++) MemType[address >> 4] = MF_RAM8;
        memset(MemCPU + 0x7000, 0x00, 0x1000);
    }

    // MBX: 1K of RAM plus its own bank register at >6FFE.
    if ((myConfig.cartType == CART_TYPE_MBX_NO_RAM) || (myConfig.cartType == CART_TYPE_MBX_WITH_RAM))
    {
        for (u32 address = 0x6000; address < 0x7000; address++) MemType[address >> 4] = MF_CART_NB;
        for (u32 address = 0x7000; address < 0x8000; address++) MemType[address >> 4] = MF_CART;

        if (myConfig.cartType == CART_TYPE_MBX_WITH_RAM)
        {
            for (u32 address = 0x6C00; address < 0x7000; address++)
            {
                MemType[address >> 4] = MF_RAM8;
                MemCPU[address] = 0x00;
            }
        }
        MemType[0x6FFE >> 4] = MF_MBX;
        MemType[0x6FFF >> 4] = MF_MBX;
        WriteBankMBX(0);
    }
}

int ti99_load_cart(const char *path, u8 initDisks, char *errorMessage, size_t errorMessageSize)
{
    if (!biosLoaded)
    {
        snprintf(errorMessage, errorMessageSize, "BIOS not loaded");
        return -1;
    }

    TMS9900_Reset();
    ti99_reset(initDisks);

    // Console ROM and GROMs go in on every load, cart or not. TMS9900_Reset() has just
    // wiped the memory type map, and a cart may have overwritten the system GROMs with
    // a '0' file last time round, so re-read both from the SD card.
    if (reload_console_images() != 0)
    {
        snprintf(errorMessage, errorMessageSize, "Cannot re-read console ROM/GROM");
        return -1;
    }

    // No cartridge: leave the slot reading 0xFF and boot straight to the console's
    // own title screen, where TI BASIC is waiting.
    if (path == NULL || path[0] == 0)
    {
        ti99_cart_alloc(0x2000);
        memset(&MemCPU[0x6000], 0xFF, 0x2000);
        tms9900.bankMask = 0x0000;
        tms9900.cartBankPtr = MemCPU + 0x6000;
        TMS9900_Kickoff();
        printf("[ti99] booting with no cartridge (TI BASIC available)\n");
        return 0;
    }

    strncpy(tmpBuf, path, MAX_PATH - 1);
    tmpBuf[MAX_PATH - 1] = 0;

    const char *ext = strrchr(path, '.');
    u16 numCartBanks = 1;
    pCodeEmulation = 0;

    if (ext && strcasecmp(ext, ".rpk") == 0)
    {
        // rpk_load() sizes and fills MemCART itself via ti99_cart_alloc().
        if (rpk_load(tmpBuf) != 0)
        {
            memset(&MemCPU[0x6000], 0xFF, 0x2000);
            snprintf(errorMessage, errorMessageSize, "Error loading RPK");
            return -1;
        }
    }
    else
    {
        u8 fileType = (u8)toupper((int)tmpBuf[strlen(tmpBuf) - 5]);

        // Size the cart buffer from the part that actually lands in it. The menu may
        // have been pointed at the 'G' file, which goes to MemGROM and can be much
        // smaller than the ROM - sizing from that would truncate the cartridge.
        long romSize = 0;
        u32  extra   = 0;
        if ((fileType == 'C') || (fileType == 'G') || (fileType == 'D'))
        {
            size_t len = strlen(tmpBuf);
            char saved = tmpBuf[len - 5];
            tmpBuf[len - 5] = 'C';
            romSize = ti99_file_size(tmpBuf);           // the 'C' file, if there is one
            tmpBuf[len - 5] = 'D';
            long dSize = ti99_file_size(tmpBuf);
            tmpBuf[len - 5] = saved;
            if (dSize > 0 && romSize < 0x2000) romSize = 0x2000;

            // A 'D' file is extracted to MemCART+0x2000, so this layout needs one 8K
            // bank beyond the 'C' file. Only this layout: a single-file image is read
            // in at offset 0 and is already the whole cartridge.
            extra = 0x2000;
        }
        else
        {
            romSize = ti99_file_size(tmpBuf);           // single-file image
        }
        if (romSize < 0x2000) romSize = 0x2000;
        file_size = (u32)romSize;

        // Adding the 'D' bank unconditionally would round a 32K image up to a 64K
        // allocation - 40K is five 8K banks, and the bank mask rounds that to eight.
        // On a board with no PSRAM that doubling is the difference between fitting in
        // the heap and not fitting in it.
        if (ti99_cart_alloc((u32)romSize + extra) != 0)
        {
            snprintf(errorMessage, errorMessageSize, "No memory for %ldK cart",
                     romSize / 1024);
            return -1;
        }

        if ((fileType == 'C') || (fileType == 'G') || (fileType == 'D'))
        {
            tms9900.bankMask = 0x003F;

            u32 numRead = load_part(tmpBuf, 'C', MemCART, MAX_CART_SIZE);
            if (numRead <= 0x2000)
            {
                // A single 8K bank. Upstream copies it into eight banks and sets a
                // matching mask; masking to bank 0 instead has the same effect - every
                // bank switch lands back on the one bank - without the seven copies.
                tms9900.bankMask = 0x0000;
            }
            else
            {
                numCartBanks = (numRead / 0x2000) + ((numRead % 0x2000) ? 1 : 0);
                tms9900.bankMask = BankMasks[numCartBanks - 1];
            }

            if (load_part(tmpBuf, 'D', MemCART + 0x2000, 0x2000) > 0)
                tms9900.bankMask = 0x0001;    // a 'D' file is always exactly one extra bank

            memcpy(&MemCPU[0x6000], MemCART, 0x2000);
            load_part(tmpBuf, 'G', &MemGROM[0x6000], 0xA000);   // up to 40K of cart GROM
        }
        else if (fileType != '0')
        {
            // Single-file multi-bank image ('8' non-inverted, '9'/'3' inverted).
            FILE *f = fopen(tmpBuf, "rb");
            if (!f)
            {
                snprintf(errorMessage, errorMessageSize, "Cannot open cartridge");
                return -1;
            }
            u32 numRead = (u32)fread(MemCART, 1, MAX_CART_SIZE, f);
            fclose(f);

            numCartBanks = (numRead / 0x2000) + ((numRead % 0x2000) ? 1 : 0);
            if (numCartBanks < 1) numCartBanks = 1;
            tms9900.bankMask = BankMasks[numCartBanks - 1];

            if ((numCartBanks > 1) && ((fileType == '9') || (fileType == '3')))
            {
                for (u16 i = 0; i < numCartBanks / 2; i++)
                {
                    memcpy(fileBuf, MemCART + (i * 0x2000), 0x2000);
                    memcpy(MemCART + (i * 0x2000), MemCART + ((numCartBanks - i - 1) * 0x2000), 0x2000);
                    memcpy(MemCART + ((numCartBanks - i - 1) * 0x2000), fileBuf, 0x2000);
                }
            }

            memcpy(&MemCPU[0x6000], MemCART, 0x2000);
            load_part(tmpBuf, 'G', &MemGROM[0x6000], 0xA000);
        }

        // A '0' file replaces the console GROMs outright (Star Trek and friends).
        load_part(tmpBuf, '0', &MemGROM[0x0000], 0x6000);

        apply_cart_type();
    }

    // Super Cart carries 32K of its own banked RAM. The cart type is only final here:
    // the .bin path sets it from the user's configuration, the .rpk path from the
    // layout the loader just parsed.
    if (myConfig.cartType == CART_TYPE_SUPERCART && MemSuperCartRAM == NULL)
    {
        // 32K, and taking it out of SRAM would mean an unguarded malloc of that size on
        // a heap that is already mostly spoken for - which panics rather than failing.
        // PSRAM instead, and a clear error when there is none.
        MemSuperCartRAM = (u8 *)ti99_psram_alloc(0x8000);
        if (MemSuperCartRAM) memset(MemSuperCartRAM, 0x00, 0x8000);
        else
        {
            snprintf(errorMessage, errorMessageSize, "Super Cart needs a PSRAM board");
            return -1;
        }
    }

    tms9900.cartBankPtr = MemCPU + 0x6000;

    if (initDisks) ti99_mount_matching_disks(path);

    TMS9900_Kickoff();
    return 0;
}

// -------------------------------------------------------------------------------------
// Auto-mount .dsk images sitting next to the cartridge, so a game that expects DSK1
// finds it. Both naming conventions upstream supports are checked:
//   GAME.bin -> GAME1.dsk / GAME2.dsk / GAME3.dsk   (replacing the last char)
//   GAME.bin -> GAME1.dsk ...                       (appended before the extension)
// -------------------------------------------------------------------------------------
void ti99_mount_matching_disks(const char *cartPath)
{
    if (!diskDsrLoaded || !cartPath || !cartPath[0]) return;

    char base[MAX_PATH];
    strncpy(base, cartPath, MAX_PATH - 1);
    base[MAX_PATH - 1] = 0;

    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;

    // Split into directory and filename for disk_mount().
    char dir[MAX_PATH];
    strncpy(dir, cartPath, MAX_PATH - 1);
    dir[MAX_PATH - 1] = 0;
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, "");

    for (u8 drive = DSK1; drive < MAX_DSKS; drive++)
    {
        snprintf(tmpBuf, MAX_PATH, "%s%c.dsk", base, '1' + drive);
        if (ti99_file_exists(tmpBuf))
        {
            char *name = strrchr(tmpBuf, '/');
            disk_mount(drive, dir, name ? name + 1 : tmpBuf);
            printf("[ti99] mounted DSK%d: %s\n", drive + 1, tmpBuf);
        }
    }
}
