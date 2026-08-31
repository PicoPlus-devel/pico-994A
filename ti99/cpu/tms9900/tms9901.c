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

#include "ti99_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ti99_fileio.h"
#include "tms9901.h"
#include "tms9900.h"
#include "../../disk.h"
#include "../../pcode.h"
#include "../../SAMS.h"
#include "../../cassette.h"

// From https://www.unige.ch/medecine/nouspikel/ti99/tms9901.htm
//
// The main CRU handling is for the lower 32 bits defined as follows:
//
// Bit 0 is used to select the timer mode. When it equals 1, the TMS9901 is in timer mode and bits 1-15 have a special meaning (see below),
// when 0 it starts the timer (if needed) and bits 1-15 are used to control I/O pins, just as bits 16-31.

// Bits 1-15 are used to read the status of the 15 interrupt pins (whether they are used as interrupt pin or as input pin).
// Writing to one of these bits does not output any data, but sets the interrupt mask for the corresponding pin: writing a 1 results
// in issuing interrupts when the pin is held low. The interrupt trigger is synchronized by the PHI* pin.

// Bits 16-31 are used to read the status of the 15 programmable I/O pins, provided they are used as input (or interrupt) pins.
// Note that the 8 versatile pins INT7*/P15 through INT15*/P7 can be read either with bits 7-15 or with bits 23-31. Writing to
// CRU bits 16-31 turns the corresponding pins into output pins and places the bit values on the pins.

// From https://www.unige.ch/medecine/nouspikel/ti99/tms9901.htm
//    =   .   ,   M   N   /  fire1  fire2
// space  L   K   J   H   ;  left1  left2
// enter  O   I   U   Y   P  right1 right2
// (none) 9   8   7   6   0  down1  down2
// fctn   2   3   4   5   1  up1    up2      [AlphaLock]
// shift  S   D   F   G   A  (none) (none)
// ctrl   W   E   R   T   Q  (none) (none)
// (none) X   C   V   B   Z  (none) (none)

// Alpha Lock is special... it's enabled via CRU[>15] and is read back on row 4... so it screws up joysticks when enabled. Sigh.


// ------------------------------------------------------------------------------------------------------------
// TMS Keys form an 8x8 matrix to mirror the spec above and can be scanned using the 3 keyboard column bits
// ------------------------------------------------------------------------------------------------------------
const u8 TIKeys[8][8] =
{
    { TMS_KEY_EQUALS,   TMS_KEY_PERIOD, TMS_KEY_COMMA, TMS_KEY_M,   TMS_KEY_N,   TMS_KEY_SLASH,  TMS_KEY_JOY1_FIRE,     TMS_KEY_JOY2_FIRE   },
    { TMS_KEY_SPACE,    TMS_KEY_L,      TMS_KEY_K,     TMS_KEY_J,   TMS_KEY_H,   TMS_KEY_SEMI,   TMS_KEY_JOY1_LEFT,     TMS_KEY_JOY2_LEFT   },
    { TMS_KEY_ENTER,    TMS_KEY_O,      TMS_KEY_I,     TMS_KEY_U,   TMS_KEY_Y,   TMS_KEY_P,      TMS_KEY_JOY1_RIGHT,    TMS_KEY_JOY2_RIGHT  },
    { TMS_KEY_NONE,     TMS_KEY_9,      TMS_KEY_8,     TMS_KEY_7,   TMS_KEY_6,   TMS_KEY_0,      TMS_KEY_JOY1_DOWN,     TMS_KEY_JOY2_DOWN   },
    { TMS_KEY_FUNCTION, TMS_KEY_2,      TMS_KEY_3,     TMS_KEY_4,   TMS_KEY_5,   TMS_KEY_1,      TMS_KEY_JOY1_UP,       TMS_KEY_JOY2_UP     },
    { TMS_KEY_SHIFT,    TMS_KEY_S,      TMS_KEY_D,     TMS_KEY_F,   TMS_KEY_G,   TMS_KEY_A,      TMS_KEY_NONE,          TMS_KEY_NONE        },
    { TMS_KEY_CONTROL,  TMS_KEY_W,      TMS_KEY_E,     TMS_KEY_R,   TMS_KEY_T,   TMS_KEY_Q,      TMS_KEY_NONE,          TMS_KEY_NONE        },
    { TMS_KEY_NONE,     TMS_KEY_X,      TMS_KEY_C,     TMS_KEY_V,   TMS_KEY_B,   TMS_KEY_Z,      TMS_KEY_NONE,          TMS_KEY_NONE        },
};

// ---------------------------------------------------------------------------------
// A note on pin aliasing, because upstream had a look-up table here that was wrong.
//
// CRU read bits 1-15 report the interrupt lines and bits 16-31 report the 16 I/O
// pins P0-P15. The eight versatile pins are shared - INT7*/P15 down to INT15*/P7 -
// so bit 23 (P7) and bit 15 (INT15*) are the same piece of silicon. But they are
// still separate CRU addresses, and a read of bit 16+n returns P(n) whatever else
// that pin is called. For an output pin that is the last value written to it.
//
// The TI-99/4A never reads the versatile pins through their interrupt addresses:
// bits 1-2 are the interrupt lines, 3-10 are the keyboard matrix, and the cassette
// port lives at 22-27. So no aliasing is needed at all - reads of 16-31 are plain
// loopback, which is what the cassette DSR depends on when it reads back the motor
// and audio-gate bits it just set.
//
// Upstream mapped 23->15, 24->16 ... 31->23 (a flat -8), so a read of bit 27 - the
// cassette input - returned pin 19, which is PIN_COL2, the keyboard column select.
// Nothing read those bits before cassette support, which is why it never showed.
// ---------------------------------------------------------------------------------

// --------------------------------------------------------------------------------------------
// The entire TMS9901 struct and state information is placed into .DTCM fast memory on the DS
// so that it's as fast as possible. We will also try to put as much of the CRU logic into the
// .ITCM fast instruction memory to speed up that processing to help the poor DS CPU along...
// --------------------------------------------------------------------------------------------
TMS9901 tms9901      DTCM_DATA;


// --------------------------------------------------------------------
// Clears out the TMS9901 and clears out any pending interrupts...
// --------------------------------------------------------------------
void TMS9901_Reset(void)
{
    // Clear out the entire state of the TMS9901 - this will also force all pins LOW
    memset(&tms9901, 0x00, sizeof(tms9901));

    // -------------------------------------------------------------------------------------------------------------------
    // Set the state of the 32 I/O pins... with the first pin being special to indicate timer active or IO mode active
    // -------------------------------------------------------------------------------------------------------------------
    tms9901.PinState[PIN_TIMER_OR_IO]  =  IO_MODE;
    tms9901.TimerLoadCycle             =  tms9900.cycles;

    TMS9900_ClearInterrupt(0xFFFF);
}

// -----------------------------------------------------------------------------------------
// Bring TimerCounter up to date with the CPU cycle count.
//
// The 9901 decrementer ticks once every 64 CPU clocks and runs continuously - it does not
// stop when the CPU drops into clock mode to look at it. What clock mode does is latch the
// current value into the register reads come from (TimerLatch below). That distinction
// matters here: the cassette DSR uses the timer as a stopwatch, entering and leaving clock
// mode on every measurement, so anything that reset the count or dropped the accumulated
// remainder on each read would make the clock run slow by up to a tick every time.
//
// Upstream decremented by a flat 3 every scanline, which averages out to about the right
// frequency but quantises every read to a 63.7us boundary in steps of 3 ticks. Against a
// bit cell of roughly 700us that is enough jitter to corrupt the decode, so the counter
// is now derived from tms9900.cycles instead of stepped.
//
// tms9900.cycles is a free-running u32 that wraps every ~24 minutes at 3MHz, hence the
// unsigned delta - never compare the absolute values.
// -----------------------------------------------------------------------------------------
void TMS9901_TimerSnapshot(void)
{
    if (tms9901.TimerStart == 0) return;                            // No timer programmed

    u32 elapsed = (u32)(tms9900.cycles - tms9901.TimerLoadCycle) >> 6;  // 64 CPU clocks per tick
    if (elapsed == 0) return;

    tms9901.TimerLoadCycle += (elapsed << 6);                       // Keep the sub-tick remainder

    // The decrementer counts TimerStart down to 0 and reloads, so one full pass is
    // TimerStart+1 ticks. Anything past the current count has wrapped at least once.
    u32 span = tms9901.TimerStart + 1;
    if (elapsed >= tms9901.TimerCounter + 1)
    {
        TMS9901_RaiseTimerInterrupt();
        elapsed -= (tms9901.TimerCounter + 1);
        tms9901.TimerCounter = tms9901.TimerStart - (elapsed % span);
    }
    else
    {
        tms9901.TimerCounter -= elapsed;
    }
}

// -----------------------------------------------------------------------------------------
// Drive one of the 32 I/O pins and run whatever hangs off it.
//
// Reached from two places: a plain write while in I/O mode, and a write to a pin above 15
// while in clock mode. The 9901 does both things on that second case - it drops out of
// clock mode *and* drives the pin - but upstream only did the first, so the data bit was
// discarded. That went unnoticed until cassette support arrived, because clock mode is
// only used by the cassette DSR, which times its bit cells against the timer and writes
// the data line while it is in there. Every data bit of a SAVE was being swallowed.
// -----------------------------------------------------------------------------------------
static inline void TMS9901_WritePin(u8 cruA, u8 dataBit)
{
    tms9901.PinState[cruA] = dataBit;

    if (cruA == PIN_TIMER_INT)
    {
        // Any write to pin 3 will clear the timer interrupt
        TMS9901_ClearTimerInterrupt();
    }
    else if (cruA == PIN_VDP_INT && dataBit) // Are we unmasking... Need to pass through the interrupt state (River Rescue requires this)
    {
        if (tms9901.VDPIntteruptInProcess) TMS9900_RaiseInterrupt(INT_VDP); else TMS9900_ClearInterrupt(INT_VDP);
    }
    else if (cruA >= PIN_CS1_MOTOR && cruA <= PIN_TAPE_OUT)
    {
        // Cassette: motor relays, audio gate and the data line out. The pin state is
        // already stored above (the DSR reads bits 22-25 back), so the deck only needs
        // telling that something changed.
        cassette_cru_write(cruA, dataBit);
    }
}

// -----------------------------------------------------------------------------------------
// Write up to 16 bits of information to the CRU. This routine handles the data shifting
// as needed to clock out one or more bits (up to the full 16 bits) to the CRU. The CPU
// calls that bring us here will already have shifted down the cruAddress so we're dealing
// with 0-31 for the main CRU bits and adjust appropriately for peripheral CRU use.
//
// The following is the typical TI-99/4a use of CRU address ranges:
//      >0000-07FE   Internal Use (the 32 main CRU bits are mapped here - mirrored)
//      >0800-0FFE   Reserved (CRU paging uses this... SuperSpace II and some Databiotics carts)
//      >1000-10FE   Horizon RAMDisk or IDE Harddisk
//      >1100-11FE   Disk Controller
//      >1200-12FE   Reserved
//      >1300-13FE   RS-232 (Primary)
//      >1400-14FE   Unassigned
//      >1500-15FE   RS-232 (Secondary)
//      >1600-16FE   Unassigned
//      >1700-17FE   HEX-BUS Interface
//      >1800-18FE   Thermal Printer
//      >1900-19FE   Reserved
//      >1A00-1AFE   Unassigned
//      >1B00-1BFE   Unassigned
//      >1C00-1CFE   Video Controller Card
//      >1D00-1DFE   IEEE 488 Bus Controller Card
//      >1E00-1EFE   AMS/SAMS
//      >1F00-1FFE   P-Code Card
// -----------------------------------------------------------------------------------------
ITCM_CODE void TMS9901_WriteCRU(u16 cruAddress, u16 data, u8 num)
{
    if (num == 0) num = 16;     // A zero means write all 16 bits...

    for (u8 bitNum = 0; bitNum < num; bitNum++)
    {
        u16 dataBit = (data & (1<<bitNum)) ? 1:0;  // Get the status of this data bit

        // --------------------------------------------------------------------------------------
        // Check to see if we're in the external peripheral area - this is for Disk DSR and SAMS
        // --------------------------------------------------------------------------------------
        if (cruAddress & 0xFC00) // At or above CRU base >800?
        {
            if ((cruAddress & 0xF80) == (0x1100 >> 1))       // Disk support at CRU base >1100
            {
                disk_cru_write(cruAddress, dataBit);
            }
            else if ((cruAddress & 0xFFE) == (0x1E00 >> 1))  // SAMS support at CRU base >1E00
            {
                SAMS_cru_write(cruAddress, dataBit);
            }
            else if ((cruAddress & 0xF80) == (0x1F00 >> 1))  // P-Code support at CRU base >1F00
            {
                pcode_cru_write(cruAddress, dataBit);
            }
            else if ((cruAddress & 0xF80) == (0x800 >> 1))   // Cart-based CRU bankswitching at CRU base >800
            {
                cart_cru_write(cruAddress, dataBit);
            }
        }
        else  // This is the internal console CRU bits below CRU base >800... the famous 32 CRU bits that must be handled in either TIMER mode or IO mode.
        {
            u8 cruA = cruAddress & 0x1F; // Map down to 32 bits...

            // -------------------------------------------------------------------------------------------------
            // Bit 0 is special as it defines if we are in Timer or I/O mode. The cassette DSR flips into
            // clock mode to read the decrementer as a stopwatch, so the two transitions matter: going in
            // latches the current count, coming out restarts it from where the latch left off.
            // -------------------------------------------------------------------------------------------------
            if (cruA == PIN_TIMER_OR_IO)
            {
                u8 newMode = (dataBit ? TIMER_MODE : IO_MODE);
                if (newMode == TIMER_MODE)
                {
                    // Entering clock mode latches the live decrementer into the register
                    // the CPU reads. The decrementer itself carries on regardless.
                    TMS9901_TimerSnapshot();
                    tms9901.TimerLatch = tms9901.TimerCounter;
                }
                tms9901.PinState[PIN_TIMER_OR_IO] = newMode;
            }
            else
            if (tms9901.PinState[PIN_TIMER_OR_IO] == TIMER_MODE)
            {
                // --------------------------------------------------------------------------------------
                // In Timer Mode we need to handle the bit15 soft reset as well as setting timer data
                // --------------------------------------------------------------------------------------
                if (cruA == 15) // Soft Reset
                {
	                tms9901.PinState[0]=0;	// timer control
	                tms9901.PinState[1]=0;	// peripheral interrupt mask
	                tms9901.PinState[2]=0;	// VDP interrupt mask
	                tms9901.PinState[3]=0;	// timer interrupt mask
	                tms9901.TimerCounter=0; // timer counter
                    tms9901.TimerStart=0;   // timer start
                }
                else if ((cruA >= 1) && (cruA <= 14))    // Bits 1-14 represent the the 14 bit counter/timer ...
                {
                    if (dataBit) tms9901.TimerStart |= (1 << (cruA-1));     // Clear or set bit in Timer
                    else tms9901.TimerStart &= ~(1 << (cruA-1));
                    tms9901.TimerStart &= 0x3FFF;                           // 14 bits of Timer
                    tms9901.TimerCounter = tms9901.TimerStart;              // Timer will countdown only in IO mode
                    tms9901.TimerLatch = tms9901.TimerStart;                // reads see the new value at once
                    tms9901.TimerLoadCycle = tms9900.cycles;                // ... measured from right now
                    TMS9900_SetAccurateEmulationFlag(ACCURATE_EMU_TIMER);   // Force timer to be dealt with...
                }
                else if (cruA > 15)
                {
                    // A write above pin 15 drops the chip out of clock mode - and still
                    // lands on the pin. See TMS9901_WritePin above for why that matters.
                    tms9901.PinState[PIN_TIMER_OR_IO] = IO_MODE;
                    TMS9901_WritePin(cruA, dataBit);
                }
            }
            else    // We're in I/O Mode
            {
                // --------------------------------------------------------------------------------------
                // Just save the data bit (0 or 1) for the pin in I/O mode. We can decode the keyboard
                // column and alpha-lock easily enough with the use of defines from tms9901.h
                // --------------------------------------------------------------------------------------
                TMS9901_WritePin(cruA, dataBit);
            }
        }
        cruAddress++;   // Move to the next CRU bit (if any)
    }
}

// --------------------------------------------------------------------------------------------------
// Read from 1 to 16 bits of CRU information from the desired CRU address. This address has already
// been shifted down 1 by the CPU core that called it... 
// --------------------------------------------------------------------------------------------------
ITCM_CODE u16 TMS9901_ReadCRU(u16 cruAddress, u8 num)
{
    u16 retVal = 0x0000;        // Accumulate bits below

    if (num == 0) num = 16;     // A zero means read all 16 bits...

    for (u8 bitNum = 0; bitNum < num; bitNum++)
    {
        u8 bitState = 1;        // Default output to a '1' until proven otherwise below...
        
        if (cruAddress & 0xFC00)
        {
            if ((cruAddress & 0xF80) == 0x880)    // Disk support from >880 to >888 (CRU base >1000)
            {
                bitState = disk_cru_read(cruAddress);
            }
            else if ((cruAddress & 0xF80) == 0xF00)  // SAMS support at >F00 and >F01 (CRU base >1E00)
            {
                bitState = SAMS_cru_read(cruAddress);
            }
            else if ((cruAddress & 0xF80) == 0x400)  // Cart-based CRU bankswitching... (CRU base >800)
            {
                bitState = cart_cru_read(cruAddress);
            }
        }
        else
        {
            // The pin number is kept in its own local. Upstream aliased in place and then
            // incremented the aliased value at the bottom of the loop, so a multi-bit STCR
            // reaching into the aliased region walked the wrong addresses from the second
            // bit on. Latent while only the identity-mapped keyboard bits were ever read.
            u8 cruA = cruAddress & 0x1F;     // CRU mirrors

            // Clock mode only changes what bits 0-15 mean. Bits 16-31 are the I/O pins
            // whatever mode the chip is in - and that matters as much as the write side
            // did: the cassette DSR times its bit cells against the timer, so it polls the
            // tape input from inside clock mode. Upstream ran the clock decode over all 32
            // bits, so a read of bit 27 came back as a bit of the timer register and the
            // tape was never seen at all. Reading does not drop out of clock mode - only
            // a write above pin 15 does that.
            if (tms9901.PinState[PIN_TIMER_OR_IO] == TIMER_MODE && cruA <= 15)
            {
                switch (cruA)
                {
                    case 0:     bitState = 1;                                                                               break;     // Bit 0 in timer mode always returns '1'
                    case 15:    bitState = ((tms9901.VDPIntteruptInProcess && tms9901.PinState[PIN_VDP_INT]) ||
                                            (tms9901.TimerIntteruptInProcess && tms9901.PinState[PIN_TIMER_INT]) ? 0:1);    break;     // Pin 15 is for either VDP or Timer interrupt... report if that's set
                    default:    bitState = (tms9901.TimerLatch & (1<<(cruA-1))) ? 1:0;                                      break;     // Otherwise report the latched timer bit
                }
            }
            else if (cruA == PIN_TAPE_IN)
            {
                // ------------------------------------------------------------------------------
                // Cassette input. Read ahead of the alias table on purpose: bit 27 is P11, which
                // aliases to read bit 11, and routing it through the table would put it back in
                // the keyboard/interrupt decode below. With no tape mounted this returns the
                // idle level, so a cart that pokes at the CRU sees what it always did.
                // ------------------------------------------------------------------------------
                bitState = cassette_read_bit();
            }
            else    // This is IO mode - there are some aliased pins we need to be careful of...
            {
                // --------------------------------------------------
                //0 >0000   I/O 0: I/O mode 1: timer mode
                //1 >0002   I+  Peripheral interrupt incoming line
                //2 >0004   I+  VDP interrupts incoming line
                // --------------------------------------------------

                switch (cruA)
                {
                    case 0:     bitState = 0;                                           break;      // Bit 0 in IO mode always returns '0'

                    case 1:     bitState = 1;                                           break;      // We don't allow external interrupts
                    case 2:     bitState = (tms9901.VDPIntteruptInProcess ? 0 : 1);     break;      // Pin 2 is for reporting the VDP interrupt

                    case 7:     // Keyboard Row 4
                                if (tms9901.PinState[PIN_ALPHA_LOCK] == PIN_LOW)
                                {
                                    // The Alpha Lock is scanned on row 4 but only when the Alpha Lock scanning pin was driven LOW
                                    if (tms9901.CapsLock) bitState = 0;
                                }
                                // No break is INTENTIONAL!
                    case 3:     // Keyboard Row 0
                    case 4:     // Keyboard Row 1
                    case 5:     // Keyboard Row 2
                    case 6:     // Keyboard Row 3
                    case 8:     // Keyboard Row 5
                    case 9:     // Keyboard Row 6
                    case 10:    // Keyboard Row 7
                        {
                            // ------------------------------------------------------------------------------------------------------------------------
                            // This handles both Keybaord and Joystick (P1 and P2) inputs in a unified manner... to the TI-99/4a, it's all the same.
                            // ------------------------------------------------------------------------------------------------------------------------
                            u8 column = (tms9901.PinState[PIN_COL3]<<2) | (tms9901.PinState[PIN_COL2]<<1) | (tms9901.PinState[PIN_COL1]<<0);
                            if (tms9901.Keyboard[TIKeys[cruA-3][column]]) bitState = 0;
                        }
                        break;

                    default:    bitState = tms9901.PinState[cruA]; break;                             // Otherwise loopback: returned bit will be last value written
                }
            }
        }
        
        // ----------------------------------------------------------------------------------------------
        // If we've decoded a '1' write that bit into the 16-bit return value into the appropriate spot.
        // ----------------------------------------------------------------------------------------------
        if (bitState)
        {
            retVal |= (1 << bitNum);
        }
        cruAddress++;   // Move to the next CRU bit (if any)
    }

    return retVal;  // Return the assembled word to the caller...
}

// -----------------------------------------------------------------------------------------
// On each pass of the main loop (in DS99.c) we want to clear out the joystick and
// keyboard information and re-accumulate any joystick presses or keyboard presses.
// -----------------------------------------------------------------------------------------
void TMS9901_ClearJoyKeyData(void)
{
    memset(tms9901.Keyboard,  0x00, sizeof(tms9901.Keyboard));
}

// -----------------------------------------------------------------------------------------
// Handle VDP Interrupt
// -----------------------------------------------------------------------------------------
void TMS9901_RaiseVDPInterrupt(void)
{
    if (!tms9901.VDPIntteruptInProcess)                     // Do nothing if we've already fired this interrupt...
    {
        tms9901.VDPIntteruptInProcess = 1;                  // Remember that we raised this interrupt
        if (tms9901.PinState[PIN_VDP_INT] == PIN_HIGH)      // Raise the interrupt if the CPU wants to see it
        {
            TMS9900_RaiseInterrupt(INT_VDP);                // Tell the TMS9900 that we have an interrupt...
        }
    }
}

void TMS9901_ClearVDPInterrupt(void)
{
    if (tms9901.VDPIntteruptInProcess)
    {
        tms9901.VDPIntteruptInProcess = 0;                  // Remember that we cleared this interrupt
        if (tms9901.PinState[PIN_VDP_INT] == PIN_HIGH)      // Clear the interrupt if the CPU wants to see it
        {
            TMS9900_ClearInterrupt(INT_VDP);                // Tell the TMS9900 that we have cleared an interrupt...
        }
    }
}

// -----------------------------------------------------------------------------------------
// Handle Timer Interrupt
// -----------------------------------------------------------------------------------------
void TMS9901_RaiseTimerInterrupt(void)
{
    if (!tms9901.TimerIntteruptInProcess)                     // Do nothing if we've already fired this interrupt...
    {
        tms9901.TimerIntteruptInProcess = 1;                  // Remember that we raised this interrupt
        if (tms9901.PinState[PIN_TIMER_INT] == PIN_HIGH)      // Raise the interrupt if the CPU wants to see it
        {
            TMS9900_RaiseInterrupt(INT_TIMER);                // Tell the TMS9900 that we have an interrupt...
        }
    }
}

void TMS9901_ClearTimerInterrupt(void)
{
    if (tms9901.TimerIntteruptInProcess)
    {
        tms9901.TimerIntteruptInProcess = 0;                  // Remember that we cleared this interrupt
        if (tms9901.PinState[PIN_TIMER_INT] == PIN_HIGH)      // Clear the interrupt if the CPU wants to see it
        {
            TMS9900_ClearInterrupt(INT_TIMER);                // Tell the TMS9900 that we have cleared an interrupt...
        }
    }
}

// End of file...
