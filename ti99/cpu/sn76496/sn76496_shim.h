// =====================================================================================
// sn76496_shim.h - FluBBa API on top of the C SN76489/76496 core.
//
// DS994a drives Fredrik Ahlstrom's SN76496 emulator, which ships as ARM32 assembly
// (SN76496.s) and therefore cannot build for Cortex-M33. The chip is the same one the
// Sega Master System uses, so this maps the two calls the TI core makes onto the C
// implementation already shipping in pico-smsplus (smsplus/sn76496.c, unmodified).
//
// The TI-99/4A's TMS9919 / SN94624 runs at the same 3.579545 MHz as the SMS PSG.
// =====================================================================================
#ifndef _SN76496_SHIM_H_
#define _SN76496_SHIM_H_

#include "sn76496.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TI99_PSG_CLOCK   3579545
#define TI99_PSG_CHIP    0

// Upstream passes a per-chip context pointer; the C core indexes a global array
// instead, so the pointer is accepted and ignored. Kept as a distinct type so the
// upstream declaration "extern SN76496 snti99;" still compiles.
typedef struct { int chip; } SN76496;
extern SN76496 snti99;

static inline void sn76496W(u8 value, SN76496 *chip)
{
    (void)chip;
    SN76496Write(TI99_PSG_CHIP, value);
}

void ti99_psg_init(int sample_rate);
void ti99_psg_mix(s16 *dest, int samples);   // mono, one frame's worth

#ifdef __cplusplus
}
#endif

#endif // _SN76496_SHIM_H_
