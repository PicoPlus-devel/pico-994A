// =====================================================================================
// ti99_compat.h - libnds replacement for the pico-994A port.
//
// The TI-99/4A emulation core in this directory comes from DS994a by
// wavemotion-dave, which targets the Nintendo DS and therefore pulls in <nds.h>
// for its fixed-width types and for the ITCM/DTCM fast-memory attributes.
// This header provides the same names on RP2350 so the upstream sources compile
// with as few edits as possible.
//
// See ti99/DS994a-README.md for the upstream copyright notice.
// =====================================================================================
#ifndef _TI99_COMPAT_H_
#define _TI99_COMPAT_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>   // libnds pulled this in; the core uses bool/true/false

#ifdef __cplusplus
extern "C" {
#endif

// -------------------------------------------------------------------------------------
// libnds fixed-width type names.
// -------------------------------------------------------------------------------------
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;

typedef volatile uint8_t  vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

// ColEM heritage: the TMS9918A driver spells its byte/word types this way.
typedef uint8_t  byte;
typedef uint16_t word;

// -------------------------------------------------------------------------------------
// Memory placement.
//
// The DS has 32K of ITCM and 16K of DTCM; the RP2350 has neither, and its 520K of
// SRAM is the binding constraint for this port (menu, framebuffer and FatFS share
// the same heap). Both attributes therefore expand to nothing by default and the
// core runs from XIP flash through the cache.
//
// If frame time needs it, define TI99_HOT_IN_RAM at build time to pull the handful
// of hottest functions into SRAM. Do that selectively rather than wholesale -
// ITCM_CODE is applied to ~25 functions upstream and moving them all would cost
// far more SRAM than is available.
// -------------------------------------------------------------------------------------
// ITCM_CODE is used as a declaration *prefix* upstream ("ITCM_CODE void foo(void)"),
// so it cannot map to the SDK's __not_in_flash_func(x), which wraps the name.
// The equivalent prefix form is the section attribute that macro expands to.
#ifdef TI99_HOT_IN_RAM
#define ITCM_CODE __attribute__((section(".time_critical.ti99"), noinline))
#else
#define ITCM_CODE
#endif

#define DTCM_DATA

#ifndef ALIGN
#define ALIGN(x) __attribute__((aligned(x)))
#endif

#ifdef __cplusplus
}
#endif

#endif // _TI99_COMPAT_H_
