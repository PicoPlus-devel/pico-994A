// =====================================================================================
// sn76496_shim.c - see sn76496_shim.h
// =====================================================================================
#include <string.h>
#include "ti99_compat.h"
#include "sn76496_shim.h"

// The upstream core declares "extern SN76496 snti99;" and passes its address to
// every write. Nothing reads it - the C core keys off the chip index instead.
SN76496 snti99 = { TI99_PSG_CHIP };

void ti99_psg_init(int sample_rate)
{
    SN76496_init(TI99_PSG_CHIP, TI99_PSG_CLOCK, 255, sample_rate);

    // SN76496Write only assigns NoiseFB when the noise control register is
    // written. A title that starts the noise channel before touching register 6
    // would otherwise shift a zero feedback value in and produce silence, so
    // seed it with white noise here.
    sn[TI99_PSG_CHIP].NoiseFB = 0x12000;
}

// Render one block of mono samples. The core splits its output into left/right by
// mask (bits 4-7 -> buffer[0], bits 0-3 -> buffer[1]); with all eight bits set both
// sides are identical, so the same destination can be handed to both and the second
// store simply rewrites the same value.
void ti99_psg_mix(s16 *dest, int samples)
{
    if (samples <= 0) return;
    signed short int *bufs[2] = { (signed short int *)dest, (signed short int *)dest };
    SN76496Update(TI99_PSG_CHIP, bufs, samples, 0xFF);
}
