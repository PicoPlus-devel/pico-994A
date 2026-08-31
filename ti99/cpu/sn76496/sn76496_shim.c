// =====================================================================================
// sn76496_shim.c - see sn76496_shim.h
// =====================================================================================
#include <string.h>
#include "ti99_compat.h"
#include "sn76496_shim.h"

// The upstream core declares "extern SN76496 snti99;" and passes its address to
// every write. Nothing reads it - the C core keys off the chip index instead.
SN76496 snti99 = { TI99_PSG_CHIP };

// DC blocker state, see ti99_psg_mix(). Q14 fixed point: see the note there about why
// this cannot be kept at sample precision.
static int psg_dc_q14 = 0;

void ti99_psg_init(int sample_rate)
{
    SN76496_init(TI99_PSG_CHIP, TI99_PSG_CLOCK, 255, sample_rate);

    psg_dc_q14 = 0;

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
//
// The core's output is **unipolar**: vol[] counts how long each square wave sits in its
// 1 position, so the channel sum only ever runs 0..MAX_OUTPUT and carries a DC offset of
// roughly half its own amplitude. On its own that is inaudible - a speaker cannot
// reproduce DC - but it costs the whole negative half of the range, and once the Speech
// Synthesizer is mixed in on top the sum only ever clips against the positive rail.
// A recording of Parsec's opening bore this out exactly: 1781 clipped samples, every one
// of them at +32767 and none at -32768, and all 1781 within reach of the ceiling purely
// because of the offset they were riding on.
//
// So block the DC before anything else sees it: track the offset with a one-pole low
// pass at about 7 Hz - far below anything the PSG produces - and subtract it.
//
// The estimate is kept in Q14 rather than at sample precision, and that detail is the
// whole ball game. Written the obvious way as a direct high pass,
//     y[n] = x[n] - x[n-1] + (1023/1024) * y[n-1]
// the feedback term is a *floor* divide on a signed value, so for any negative state
// with |y| < 1024, floor(y * 1023/1024) == y. Negative offsets are a fixed point: they
// never decay. Positive ones bleed away one LSB per sample, negative ones stay forever,
// so every sound that ended left the output parked on a DC step - which is precisely
// what crackling in an otherwise silent passage sounds like. Carrying 14 fractional bits
// leaves the residual below a fraction of one sample unit, and true digital silence in
// gives true digital silence out.
void ti99_psg_mix(s16 *dest, int samples)
{
    if (samples <= 0) return;
    signed short int *bufs[2] = { (signed short int *)dest, (signed short int *)dest };
    SN76496Update(TI99_PSG_CHIP, bufs, samples, 0xFF);

    int dc = psg_dc_q14;
    for (int i = 0; i < samples; i++)
    {
        int x = dest[i];
        dc += ((x << 14) - dc) >> 10;       // ~7 Hz at 44.1 kHz
        int y = x - (dc >> 14);
        if (y >  32767) y =  32767;
        if (y < -32768) y = -32768;
        dest[i] = (s16)y;
    }
    psg_dc_q14 = dc;
}
