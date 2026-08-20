// =====================================================================================
// ti99_pico.h - what main.cpp uses to drive the emulated machine.
// =====================================================================================
#ifndef _TI99_PICO_H_
#define _TI99_PICO_H_

#include <stddef.h>
#include "ti99_compat.h"

#ifdef __cplusplus
extern "C" {
#endif

// TI-99/4A video is 256x192 inside the framework's 320x240 active area.
#define TI99_SCREEN_W          256
#define TI99_SCREEN_H          192
#define TI99_MARGIN_LEFT       ((320 - TI99_SCREEN_W) / 2)   // 32
#define TI99_MARGIN_TOP        ((240 - TI99_SCREEN_H) / 2)   // 24
#define TI99_MARGIN_BOTTOM     TI99_MARGIN_TOP

#define TI99_AUDIO_SAMPLE_RATE 44100

// The Speech Synthesizer's two TMS6100 ROMs hold ~32K of resident vocabulary.
#define TI99_SPEECH_ROM_SIZE   (32 * 1024)

// The TMS5200 runs its lattice filter at 8kHz; the mixer resamples up to the
// framework's rate.
#define TI99_SPEECH_RATE       8000

// --- machine lifecycle ---------------------------------------------------------------
int  ti99_alloc_memory(void);
void ti99_free_memory(void);
int  ti99_load_bios(char *errorMessage, size_t errorMessageSize);
int  ti99_load_cart(const char *path, u8 initDisks, char *errorMessage, size_t errorMessageSize);
void ti99_reset(u8 initDisks);
int  ti99_disk_dsr_available(void);
void ti99_mount_matching_disks(const char *cartPath);

// --- cartridge memory (rpk.c calls these too) ----------------------------------------
int  ti99_cart_alloc(u32 size);
void ti99_cart_free(void);

#ifdef __cplusplus
}
#endif

#endif // _TI99_PICO_H_
