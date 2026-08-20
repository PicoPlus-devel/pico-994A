// =====================================================================================
// ti99.h - the interface between the ported DS994a core and the pico-994A front end.
//
// Upstream this surface was spread over DS99.h, DS99_utils.h and DS99mngt.h, mixed in
// with Nintendo DS UI declarations. Everything the emulation core actually reaches for
// is gathered here; the DS-only parts (touch screen, sprite/BG layers, sound FIFO,
// config file browser) are gone and replaced by main.cpp + pico_shared.
//
// The two config structs are kept field-for-field so the upstream core compiles
// unchanged - it reads myConfig in ~33 places.
//
// See ti99/DS994a-README.md for the upstream copyright notice.
// =====================================================================================
#ifndef _TI99_H_
#define _TI99_H_

#include "ti99_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_PATH                256

// -------------------------------------------------------------------------------------
// Machine and cartridge types (values must not change - saved in per-game config)
// -------------------------------------------------------------------------------------
#define MACH_TYPE_NORMAL32K     0
#define MACH_TYPE_SAMS_1MB      1
#define MACH_TYPE_SAMS_2MB      2
#define MACH_TYPE_SAMS_4MB      3
#define MACH_TYPE_SAMS_8MB      4

#define CART_TYPE_NORMAL        0
#define CART_TYPE_SUPERCART     1
#define CART_TYPE_MINIMEM       2
#define CART_TYPE_MBX_NO_RAM    3
#define CART_TYPE_MBX_WITH_RAM  4
#define CART_TYPE_PAGEDCRU      5

// -------------------------------------------------------------------------------------
// Per-game configuration. Same layout as upstream struct Config_t; the DS-only key
// mapping and UI fields are dropped since pico_shared owns input and settings.
// -------------------------------------------------------------------------------------
struct Config_t
{
    u32 game_crc;
    u8  frameSkip;
    u8  maxSprites;      // 4 = hardware accurate, 32 = no sprite limit
    u8  memWipe;         // how RAM is filled at power on
    u8  isPAL;           // 0 = NTSC/60Hz, 1 = PAL/50Hz
    u8  capsLock;
    u8  RAMMirrors;      // emulate the scratchpad RAM mirrors
    u8  emuSpeed;
    u8  machineType;     // MACH_TYPE_*
    u8  cartType;        // CART_TYPE_*
    u8  spriteCheck;     // sprite collision scan frequency
    u8  sounddriver;     // 2 = sample audio every scanline
};

extern struct Config_t myConfig;

// -------------------------------------------------------------------------------------
// Emulated memory. Allocated by ti99_alloc_memory() at game start and released by
// ti99_free_memory() on return to the menu - nothing here may be a static array, or
// the pico_shared menu (RomLister, screen buffer, artwork) runs out of heap on boards
// without PSRAM.
// -------------------------------------------------------------------------------------
extern u8  *MemCART;             // Cartridge ROM image, banked at >6000
extern u32  MAX_CART_SIZE;       // Bytes actually allocated for MemCART
extern u8  *SharedMemBuffer;     // Scratch: DSK3 image buffering
extern u8  *SharedMemBufferBig;  // Scratch: SAMS backing store (PSRAM boards)
extern u8  *DISK_DSR;            // 8K disk controller DSR image from /bios/
extern u8   fileBuf[0x2000];     // 8K sector cache / file I/O / CRC scratch
extern char tmpBuf[MAX_PATH];    // Path assembly scratch
extern u32  file_size;

// -------------------------------------------------------------------------------------
// Hooks the core calls out through. Implemented in ti99_pico.c / main.cpp.
// -------------------------------------------------------------------------------------
extern void TI99RenderLine(u8 y);      // one scanline is ready in XBuf
extern void TI99UpdateScreen(void);    // the frame is complete
extern void processDirectAudio(void);  // per-scanline audio sampling (sounddriver == 2)
extern void DS_Print(int x, int y, int scr, char *msg);  // status text -> UART
// PSRAM allocation for SAMS. Returns NULL on boards with no PSRAM fitted, which
// is the signal to stay on the plain 32K expansion. Implemented in main.cpp.
extern void *ti99_psram_alloc(size_t size);
extern void  ti99_psram_free(void *p);

extern void SetDiagonals(void);        // per-title input tweaks, no-ops in this port
extern void MapPlayer2(void);

// -------------------------------------------------------------------------------------
// Core entry points (upstream DS99mngt.h / tms9918a.h).
// -------------------------------------------------------------------------------------
extern u32  LoopTMS9900(void);         // run one scanline; returns 0 when frame done
extern byte Loop9918(void);

#ifdef __cplusplus
}
#endif

#endif // _TI99_H_
