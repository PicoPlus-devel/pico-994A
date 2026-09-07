// =====================================================================================
// Copyright (c) 2023-2026 Dave Bernazzani (wavemotion-dave)
//
// Copying and distribution of this emulator, its source code and associated 
// readme files, with or without modification, are permitted in any medium without 
// royalty provided this copyright notice is used and wavemotion-dave is thanked profusely.
//
// The DS994a emulator is offered as-is, without any warranty.
//
// Please see the README.md file as it contains much useful info.
// =====================================================================================

#ifndef TMS9901_H_
#define TMS9901_H_

#include "ti99_compat.h"
#include <string.h>

enum KEYS
{
    TMS_KEY_NONE,
    
    TMS_KEY_1, TMS_KEY_2, TMS_KEY_3, TMS_KEY_4, TMS_KEY_5, TMS_KEY_6, TMS_KEY_7, TMS_KEY_8, TMS_KEY_9, TMS_KEY_0,
    TMS_KEY_A, TMS_KEY_B, TMS_KEY_C, TMS_KEY_D, TMS_KEY_E, TMS_KEY_F, TMS_KEY_G, TMS_KEY_H, TMS_KEY_I, TMS_KEY_J,
    TMS_KEY_K, TMS_KEY_L, TMS_KEY_M, TMS_KEY_N, TMS_KEY_O, TMS_KEY_P, TMS_KEY_Q, TMS_KEY_R, TMS_KEY_S, TMS_KEY_T,
    TMS_KEY_U, TMS_KEY_V, TMS_KEY_W, TMS_KEY_X, TMS_KEY_Y, TMS_KEY_Z,
    
    TMS_KEY_ENTER,  TMS_KEY_SHIFT,   TMS_KEY_CONTROL, TMS_KEY_FUNCTION, TMS_KEY_SPACE,
    TMS_KEY_PERIOD, TMS_KEY_COMMA,   TMS_KEY_SLASH,   TMS_KEY_SEMI,     TMS_KEY_EQUALS,
    
    TMS_KEY_JOY1_UP, TMS_KEY_JOY1_DOWN, TMS_KEY_JOY1_LEFT, TMS_KEY_JOY1_RIGHT, TMS_KEY_JOY1_FIRE,
    TMS_KEY_JOY2_UP, TMS_KEY_JOY2_DOWN, TMS_KEY_JOY2_LEFT, TMS_KEY_JOY2_RIGHT, TMS_KEY_JOY2_FIRE,
    
    TMS_KEY_MAX
};

#define MAX_PINS        32      // 32 CRU pins that have to be handled in the TI99/4a

enum PIN_STATE
{
    PIN_LOW = 0, 
    PIN_HIGH           // Pins can either be high or low...
};

#define TIMER_MODE    PIN_HIGH
#define IO_MODE       PIN_LOW

// ---------------------------------------------------------
// Some special pins useful for keyboard decoding logic...
// ---------------------------------------------------------
#define PIN_TIMER_OR_IO     0
#define PIN_VDP_INT         2
#define PIN_TIMER_INT       3
#define PIN_COL1            18
#define PIN_COL2            19
#define PIN_COL3            20
#define PIN_ALPHA_LOCK      21

// ---------------------------------------------------------
// Cassette port. The console DSR bit-bangs these directly:
// it drives the motor relays and the data line, and times
// the bit cells against the 9901 timer below.
// PIN_TAPE_IN is read-only, the rest are write-only.
// ---------------------------------------------------------
#define PIN_CS1_MOTOR       22
#define PIN_CS2_MOTOR       23
#define PIN_AUDIO_GATE      24
#define PIN_TAPE_OUT        25
#define PIN_TAPE_IN         27

typedef struct _TMS9901
{
    u8      Keyboard[TMS_KEY_MAX];      // Main TI-99/4a Keyboard plus joystick inputs for both P1 and P2
    u8      PinState[MAX_PINS];         // The state of the 32 PINs
    u8      CapsLock;                   // Set to '1' if the Caps Lock is active
    u8      KeyColsScanned;             // Bit per keyboard column read since the front end last cleared it.
                                        // Lets a caller synthesising keypresses tell when the console has
                                        // actually looked at the matrix: KSCAN sweeps columns 0-5, but stops
                                        // dead while BASIC tokenises a line or scrolls the screen.
    u8      VDPIntteruptInProcess;      // Set to '1' if the VDP interrupt is in process
    u8      TimerIntteruptInProcess;    // Set to '1' if the Timer interrupt is in process
    u32     TimerStart;                 // The Starting value
    u32     TimerCounter;               // The 14-bit Timer Counter
    u32     TimerLoadCycle;             // tms9900.cycles when TimerCounter was last brought up to date
    u32     TimerLatch;                 // What a clock-mode read returns: the count as of entering clock mode
} TMS9901;

extern TMS9901 tms9901;

// The 8x8 key matrix the CRU read decodes through: TIKeys[row][column] holds the
// TMS_KEY_* at that position. Exposed so callers that synthesise keypresses can walk
// it - the console GROM's own translation tables are indexed by matrix position, not
// by TMS_KEY_*, so turning a character back into a key needs this.
extern const u8 TIKeys[8][8];

extern void     TMS9901_Reset(void);
extern void     TMS9901_TimerSnapshot(void);
extern void     TMS9901_WriteCRU(u16 cruAddress, u16 data, u8 num);
extern u16      TMS9901_ReadCRU(u16 cruAddress, u8 num);
extern void     TMS9901_ClearJoyKeyData(void);
extern void     TMS9901_RaiseVDPInterrupt(void);
extern void     TMS9901_ClearVDPInterrupt(void);
extern void     TMS9901_RaiseTimerInterrupt(void);
extern void     TMS9901_ClearTimerInterrupt(void);

#endif //TMS9901_H_
