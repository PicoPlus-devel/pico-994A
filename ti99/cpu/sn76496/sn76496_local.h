// Minimal replacement for pico-smsplus's shared.h, so sn76496.c builds standalone
// in this project. The core itself is unchanged from pico-smsplus/smsplus/sn76496.c.
#ifndef _SN76496_LOCAL_H_
#define _SN76496_LOCAL_H_

#include <stdint.h>
#include "sn76496.h"

typedef int16_t INT16;

// The SN76496 mixer is per-sample hot. On RP2350 XIP with the cache is fine;
// keep the hook so it can be moved to SRAM later if measurement says so.
#ifndef in_ram
#define in_ram(f) f
#endif

#endif
