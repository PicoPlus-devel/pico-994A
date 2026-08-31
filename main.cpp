// =====================================================================================
// pico-994A - Texas Instruments TI-99/4A emulator for RP2350
//
// Front end: video, audio, input, menu and the emulation loop, on top of pico_shared.
// The emulated machine lives in ti99/ (ported from DS994a - see ti99/DS994a-README.md).
//
// Part of the same family as pico-infonesPlus, pico-smsplus, pico-pacPlus,
// pico-pcePlus, pico-genesisPlus, pico-peanutGB and pico_snesPlus.
// =====================================================================================
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <algorithm>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "ff.h"
#include "tusb.h"

#include "gamepad.h"
#include "menu.h"
#include "nespad.h"
#include "wiipad.h"
#include "FrensHelpers.h"
#include "recentgames.h"
#include "settings.h"
#include "FrensFonts.h"
#include "vumeter.h"
#include "menu_settings.h"

extern "C"
{
#include "ti99_compat.h"
#include "ti99.h"
#include "ti99_pico.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cpu/tms9918a/tms9918a.h"
#include "cpu/sn76496/sn76496_shim.h"
#include "disk.h"
#include "speech.h"
#include "cassette.h"
}

// -------------------------------------------------------------------------------------
// The TI-99/4A's TMS9900 is nominally 3 MHz but spends most of its life waiting on an
// 8-bit multiplexed bus, so the effective rate is well under half a MIPS. 252 MHz is
// ample and is a multiple of 126 MHz, which HSTX builds need for correct HDMI audio
// clocking. Overclock is therefore not offered in the settings menu.
// -------------------------------------------------------------------------------------
#define EMULATOR_CLOCKFREQ_KHZ 252000

#define AUDIOBUFFERSIZE 1024

#ifndef DVI_AUDIO_GAIN_Q8
#define DVI_AUDIO_GAIN_Q8 1024
#endif
static int g_dvi_audio_gain_q8 = DVI_AUDIO_GAIN_Q8;

static uint32_t CPUFreqKHz = EMULATOR_CLOCKFREQ_KHZ;

bool isFatalError = false;
char *romName;
static bool showSettings = false;
static bool key_done = false;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;

// Marker file that boots the console with no cartridge, straight to TI BASIC.
#define TIBASIC_EXT      ".tib"
#define TIBASIC_MARKER   "TI BASIC.tib"

const int8_t g_settings_visibility_ti99[MOPT_COUNT] = {
    0,                               // Exit Game (always visible in-game)
    1,                               // Reset Game - the TI has no reset button of its own
    BOOTLOADER_BUILD,                // Return to emuLoader picker
    0,                               // Save / Restore State (not ported yet)
    1,                               // Screen Mode
    0,                               // Scanlines toggle (superseded by Screen Mode)
    HSTX,                            // Scanline Type (HSTX only)
    1,                               // FPS Overlay
    0,                               // Audio Enable
    0,                               // Frame Skip
    HSTX && ENABLEDVI,               // Display Mode (HDMI or DVI)
    (EXT_AUDIO_IS_ENABLED),          // External Audio
    1,                               // Font Color
    1,                               // Font Back Color
    ENABLE_VU_METER,                 // VU Meter
    (HW_CONFIG == 8),                // Fruit Jam Volume Control
    0,                               // DMG Palette
    0,                               // Border Mode
    0,                               // Rapid Fire on A
    0,                               // Rapid Fire on B
    0,                               // Auto Insert Disk A (FDS only)
    0,                               // Auto Swap FDS
    0,                               // FDS Disk Swap
    0,                               // Overclock - 252MHz is already plenty for a 3MHz TMS9900
    0,                               // YM Audio (SMS only)
    1,                               // Enter bootsel mode
    1,                               // Controller Test
    0,                               // Recent Games
    0,                               // USB Drive Mode - not built (FRENS_USB_MSC is off here)
    1,                               // Cassette CS1/CS2
};

// -------------------------------------------------------------------------------------
// Cassette deck, exposed to the shared settings menu through the same hook mechanism the
// NES build uses for FDS disk swapping. The menu's mode numbering is TapeMode's, so the
// wrappers exist only to match the hook signatures exactly.
// -------------------------------------------------------------------------------------
static int  cassette_hook_mode()                                     { return (int)cassette_mode(); }
static int  cassette_hook_exists(const char *name, int mode)         { return cassette_name_exists(name, (TapeMode)mode); }
static int  cassette_hook_commit(int index, int mode, const char *n) { return cassette_commit(index, (TapeMode)mode, n); }

static const MenuCassetteHooks cassetteHooks = {
    cassette_num_tapes,
    cassette_tape_name,
    cassette_selected,
    cassette_hook_mode,
    cassette_default_name,
    cassette_hook_exists,
    cassette_hook_commit,
    cassette_rewind,
    cassette_refresh_list,
};

// 256x192 is a 4:3 picture already; the 8:7 modes would stretch it wrongly.
const uint8_t g_available_screen_modes_ti99[] = {
    0, // SCANLINE_8_7
    0, // NOSCANLINE_8_7
    1, // SCANLINE_1_1
    1  // NOSCANLINE_1_1
};

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
static uint16_t wiipad_raw_cached = 0;
#endif

static DWORD prevButtons[2]{};

// -------------------------------------------------------------------------------------
// PSRAM hooks used by the SAMS expansion and by oversized cartridges (ti99.h).
// Returns NULL on boards with no PSRAM, which the core reads as "not available".
// -------------------------------------------------------------------------------------
extern "C" void *ti99_psram_alloc(size_t size)
{
    if (!Frens::isPsramEnabled()) return nullptr;
    return Frens::f_malloc(size);
}

extern "C" void ti99_psram_free(void *p)
{
    if (p) Frens::f_free(p);
}

// -------------------------------------------------------------------------------------
// Preferred-PSRAM allocation: PSRAM when the board has it, SRAM otherwise. Used for the
// buffers the machine cannot run without, so no board is excluded - unlike
// ti99_psram_alloc above, which returns NULL to gate optional features off.
// -------------------------------------------------------------------------------------
extern "C" void *ti99_mem_alloc(size_t size)
{
    return Frens::f_malloc(size);
}

extern "C" void ti99_mem_free(void *p)
{
    if (p) Frens::f_free(p);
}

// =====================================================================================
// Video
//
// The VDP renders scanlines into XBuf as 8-bit palette indices (256x192). At end of
// frame Loop9918 calls TI99UpdateScreen(), which only flags the frame as ready; the
// conversion to display pixels happens in processPerFrame() right after the pace wait,
// i.e. at the start of vblank. Doing it there rather than mid-frame means DMA never
// reads a half-converted line - the same reason pico-pacPlus defers its copy.
// =====================================================================================
static WORD ti99_palette[16];        // TI palette -> display pixel format

static void build_palette(void)
{
    for (int i = 0; i < 16; i++)
    {
        uint8_t r = TMS9918A_palette[i * 3 + 0];
        uint8_t g = TMS9918A_palette[i * 3 + 1];
        uint8_t b = TMS9918A_palette[i * 3 + 2];
#if HSTX
        // HSTX framebuffer is RGB555
        ti99_palette[i] = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
#else
        // PicoDVI framebuffer is RGB444 in the low 12 bits
        ti99_palette[i] = ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
#endif
    }
}

// One finished scanline: convert it into the matching framebuffer row, and paint the
// side borders in the backdrop colour the machine is showing right now. On line 0 the
// top and bottom margins (and the FPS digits) are painted too, so the whole 320x240
// area is refreshed once per frame.
static inline WORD *get_display_line(int y)
{
#if HSTX
    return hstx_getlineFromFramebuffer(y);
#else
    return &Frens::framebuffer[y * 320];
#endif
}

// Top and bottom margins take the backdrop colour, so the picture sits inside a border
// the same colour the machine itself shows. Repainted once per frame (on line 0) since
// a title can change the backdrop between frames.
static void __not_in_flash_func(paint_margins)(WORD backdrop)
{
    for (int y = 0; y < TI99_MARGIN_TOP; y++)
    {
        WORD *top = get_display_line(y);
        WORD *bot = get_display_line(240 - 1 - y);
        for (int x = 0; x < 320; x++) { top[x] = backdrop; bot[x] = backdrop; }
    }

    // FPS digits live in the top margin, clear of the picture, so they go on after it.
    if (settings.flags.displayFrameRate)
    {
        WORD fgc = ti99_palette[15];
        WORD bgc = ti99_palette[1];
        char d0 = (char)('0' + ((fps / 10) % 10));
        char d1 = (char)('0' + (fps % 10));
        for (int row = 0; row < 8; row++)
        {
            WORD *dst = get_display_line(8 + row) + 4;
            char s0 = getcharslicefrom8x8font(d0, row);
            char s1 = getcharslicefrom8x8font(d1, row);
            for (int b = 0; b < 8; b++) { *dst++ = (s0 & 1) ? fgc : bgc; s0 >>= 1; }
            for (int b = 0; b < 8; b++) { *dst++ = (s1 & 1) ? fgc : bgc; s1 >>= 1; }
        }
    }
}

extern "C" void __not_in_flash_func(TI99RenderLine)(u8 y)
{
    if (y >= TI99_SCREEN_H) return;

    const WORD backdrop = ti99_palette[ti99_backdrop_color & 0x0F];

    if (y == 0) paint_margins(backdrop);

    WORD *dst = get_display_line(y + TI99_MARGIN_TOP);
    const u8 *src = XBuf;
    WORD *px = dst + TI99_MARGIN_LEFT;

    for (int x = 0; x < TI99_SCREEN_W; x++)
    {
        u8 c = src[x];
        // Colour 0 is transparent on the 9918A and shows the backdrop selected in
        // VDP register 7.
        px[x] = c ? ti99_palette[c & 0x0F] : backdrop;
    }

    for (int x = 0; x < TI99_MARGIN_LEFT; x++)
    {
        dst[x] = backdrop;
        dst[320 - 1 - x] = backdrop;
    }
}

extern "C" void TI99UpdateScreen(void)
{
    if (settings.flags.displayFrameRate)
    {
        uint32_t tick_us = Frens::time_us() - start_tick_us;
        if (tick_us) fps = (1000000 - 1) / tick_us + 1;
        start_tick_us = Frens::time_us();
    }
}

// Per-scanline audio sampling is an accuracy option upstream (sounddriver == 2). This
// port mixes a whole frame at once in process_audio_frame, so nothing to do here.
extern "C" void processDirectAudio(void) { }

// =====================================================================================
// Audio
//
// One frame of PSG output per emulated frame, pushed through whichever sink this board
// has: the Fruit Jam line-out codec, the PicoDVI HDMI ring, or the HSTX data islands.
// =====================================================================================
static inline int16_t apply_dvi_gain_i32(int x)
{
    int64_t v = (int64_t)x * (int64_t)g_dvi_audio_gain_q8;
    v >>= 8;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static s16 audioFrameBuf[TI99_AUDIO_SAMPLE_RATE / 50 + 2];   // worst case is PAL (882)
static s16 tapeFrameBuf[TI99_AUDIO_SAMPLE_RATE / 50 + 2];    // cassette monitor, same shape

// How loud the tape sits in the mix, as a fraction of full scale in Q15. About -20dB,
// which is roughly where the speech synthesiser sits - audible as feedback without
// eating the headroom the PSG needs.
#define TAPE_MONITOR_LEVEL  3200

// -------------------------------------------------------------------------------------
// The Speech Synthesizer runs its filter at 8kHz, so its output is stepped through with
// a 16.16 phase accumulator and linearly interpolated up to the output rate. Doing it
// per output sample rather than resampling a block keeps the state tiny and means the
// speech stays in step even when a frame runs long.
// -------------------------------------------------------------------------------------
#define SPEECH_PHASE_STEP  (((uint32_t)TI99_SPEECH_RATE << 16) / TI99_AUDIO_SAMPLE_RATE)

static uint32_t speechPhase = 0;
static int      speechPrev = 0, speechNext = 0;

static inline int __not_in_flash_func(speech_next_sample)(void)
{
    speechPhase += SPEECH_PHASE_STEP;
    while (speechPhase >= (1u << 16))
    {
        speechPhase -= (1u << 16);
        speechPrev = speechNext;
        speechNext = SpeechGetSample();
    }
    // Linear interpolation between the two 8kHz samples either side of us.
    int frac = (int)(speechPhase & 0xFFFF);
    return speechPrev + (((speechNext - speechPrev) * frac) >> 16);
}

static inline int16_t __not_in_flash_func(mix_clamp)(int a, int b)
{
    int v = a + b;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void __not_in_flash_func(process_audio_frame)(void)
{
    const int samples = myConfig.isPAL ? (TI99_AUDIO_SAMPLE_RATE / 50)
                                       : (TI99_AUDIO_SAMPLE_RATE / 60);

    ti99_psg_mix(audioFrameBuf, samples);

    // Fold the speech synthesiser in on top of the PSG. Skipped entirely when the chip
    // is idle, which is nearly all of the time.
    if (SpeechIsTalking())
    {
        for (int i = 0; i < samples; i++)
            audioFrameBuf[i] = mix_clamp(audioFrameBuf[i], speech_next_sample());
    }
    else
    {
        speechPhase = 0;
        speechPrev = speechNext = 0;
    }

    // Cassette monitor. On a real console the DSR opens the audio gate during a load,
    // which is why you hear the tape through the TV - and it is the only sign a cassette
    // load gives that it is getting anywhere. Kept well down: the 4x output gain leaves
    // little headroom (see CHANGELOG), and during a load the PSG is silent anyway.
    if (cassette_monitor_active())
    {
        cassette_monitor_fill(tapeFrameBuf, samples);
        for (int i = 0; i < samples; i++)
        {
            int t = ((int)tapeFrameBuf[i] * TAPE_MONITOR_LEVEL) >> 15;
            audioFrameBuf[i] = mix_clamp(audioFrameBuf[i], t);
        }
    }

#if HSTX
#if EXT_AUDIO_IS_ENABLED
    bool audioJackConnected = Frens::isHeadPhoneJackConnected();
#endif
    for (int i = 0; i < samples; i++)
    {
        int l = audioFrameBuf[i];
        int r = l;
#if ENABLE_VU_METER
        if (settings.flags.enableVUMeter) addSampleToVUMeter(l);
#endif
#if EXT_AUDIO_IS_ENABLED
        if (settings.flags.useExtAudio || audioJackConnected)
        {
            EXT_AUDIO_ENQUEUE_SAMPLE(l, r);
            continue;
        }
#endif
        hstx_push_audio_sample(apply_dvi_gain_i32(l), apply_dvi_gain_i32(r));
    }
#else
#if EXT_AUDIO_IS_ENABLED
    if (settings.flags.useExtAudio)
    {
        for (int i = 0; i < samples; i++)
        {
            int s = audioFrameBuf[i];
            EXT_AUDIO_ENQUEUE_SAMPLE(s, s);
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter) addSampleToVUMeter(s);
#endif
        }
        return;
    }
#endif
    {
        auto &ring = dvi_->getAudioRingBuffer();
        for (int i = 0; i < samples; i++)
        {
            int l = apply_dvi_gain_i32(audioFrameBuf[i]);
            if (ring.getWritableSize() > 0)
            {
                auto p = ring.getWritePointer();
                *p = {static_cast<short>(l), static_cast<short>(l)};
                ring.advanceWritePointer(1);
            }
#if ENABLE_VU_METER
            if (settings.flags.enableVUMeter) addSampleToVUMeter(l);
#endif
        }
    }
#endif
}

// =====================================================================================
// Input
//
// The TMS9901 exposes keyboard and joysticks through one flat array - tms9901.Keyboard[]
// indexed by TMS_KEY_* - which the CRU read decodes by column (tms9901.c). So driving
// the real machine is just a matter of setting the right entries each frame.
//
// The TI keyboard has 48 keys and reaches everything else through FCTN and SHIFT
// combinations. All 48 are mapped directly below; on top of that, keys a PC keyboard
// has but the TI does not (arrows, backspace, delete, escape, function keys, minus)
// are synthesised as the FCTN/SHIFT combination a TI user would type.
// =====================================================================================

// A TI keypress, optionally with a modifier the TI itself would require.
struct TIKeyCombo
{
    u8 key;
    u8 modifier;    // TMS_KEY_NONE, TMS_KEY_FUNCTION or TMS_KEY_SHIFT
};

static TIKeyCombo hidKeyToTIKey(uint8_t hid)
{
    switch (hid)
    {
        // --- letters ---------------------------------------------------------------
        case HID_KEY_A: return {TMS_KEY_A, TMS_KEY_NONE};
        case HID_KEY_B: return {TMS_KEY_B, TMS_KEY_NONE};
        case HID_KEY_C: return {TMS_KEY_C, TMS_KEY_NONE};
        case HID_KEY_D: return {TMS_KEY_D, TMS_KEY_NONE};
        case HID_KEY_E: return {TMS_KEY_E, TMS_KEY_NONE};
        case HID_KEY_F: return {TMS_KEY_F, TMS_KEY_NONE};
        case HID_KEY_G: return {TMS_KEY_G, TMS_KEY_NONE};
        case HID_KEY_H: return {TMS_KEY_H, TMS_KEY_NONE};
        case HID_KEY_I: return {TMS_KEY_I, TMS_KEY_NONE};
        case HID_KEY_J: return {TMS_KEY_J, TMS_KEY_NONE};
        case HID_KEY_K: return {TMS_KEY_K, TMS_KEY_NONE};
        case HID_KEY_L: return {TMS_KEY_L, TMS_KEY_NONE};
        case HID_KEY_M: return {TMS_KEY_M, TMS_KEY_NONE};
        case HID_KEY_N: return {TMS_KEY_N, TMS_KEY_NONE};
        case HID_KEY_O: return {TMS_KEY_O, TMS_KEY_NONE};
        case HID_KEY_P: return {TMS_KEY_P, TMS_KEY_NONE};
        case HID_KEY_Q: return {TMS_KEY_Q, TMS_KEY_NONE};
        case HID_KEY_R: return {TMS_KEY_R, TMS_KEY_NONE};
        case HID_KEY_S: return {TMS_KEY_S, TMS_KEY_NONE};
        case HID_KEY_T: return {TMS_KEY_T, TMS_KEY_NONE};
        case HID_KEY_U: return {TMS_KEY_U, TMS_KEY_NONE};
        case HID_KEY_V: return {TMS_KEY_V, TMS_KEY_NONE};
        case HID_KEY_W: return {TMS_KEY_W, TMS_KEY_NONE};
        case HID_KEY_X: return {TMS_KEY_X, TMS_KEY_NONE};
        case HID_KEY_Y: return {TMS_KEY_Y, TMS_KEY_NONE};
        case HID_KEY_Z: return {TMS_KEY_Z, TMS_KEY_NONE};

        // --- digits ----------------------------------------------------------------
        // The TI's shifted number row matches a US PC keyboard exactly (!@#$%^&*()),
        // so these need no translation.
        case HID_KEY_1: return {TMS_KEY_1, TMS_KEY_NONE};
        case HID_KEY_2: return {TMS_KEY_2, TMS_KEY_NONE};
        case HID_KEY_3: return {TMS_KEY_3, TMS_KEY_NONE};
        case HID_KEY_4: return {TMS_KEY_4, TMS_KEY_NONE};
        case HID_KEY_5: return {TMS_KEY_5, TMS_KEY_NONE};
        case HID_KEY_6: return {TMS_KEY_6, TMS_KEY_NONE};
        case HID_KEY_7: return {TMS_KEY_7, TMS_KEY_NONE};
        case HID_KEY_8: return {TMS_KEY_8, TMS_KEY_NONE};
        case HID_KEY_9: return {TMS_KEY_9, TMS_KEY_NONE};
        case HID_KEY_0: return {TMS_KEY_0, TMS_KEY_NONE};

        // --- the rest of the real TI keyboard ---------------------------------------
        case HID_KEY_ENTER:
        case HID_KEY_KEYPAD_ENTER:  return {TMS_KEY_ENTER,  TMS_KEY_NONE};
        case HID_KEY_SPACE:         return {TMS_KEY_SPACE,  TMS_KEY_NONE};
        case HID_KEY_PERIOD:        return {TMS_KEY_PERIOD, TMS_KEY_NONE};
        case HID_KEY_COMMA:         return {TMS_KEY_COMMA,  TMS_KEY_NONE};
        case HID_KEY_SLASH:         return {TMS_KEY_SLASH,  TMS_KEY_NONE};
        case HID_KEY_SEMICOLON:     return {TMS_KEY_SEMI,   TMS_KEY_NONE};
        case HID_KEY_EQUAL:         return {TMS_KEY_EQUALS, TMS_KEY_NONE};

        // --- synthesised: cursor movement -------------------------------------------
        // On a TI these are FCTN+E/S/D/X, which is also what games watch for.
        case HID_KEY_ARROW_UP:      return {TMS_KEY_E, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_DOWN:    return {TMS_KEY_X, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_LEFT:    return {TMS_KEY_S, TMS_KEY_FUNCTION};
        case HID_KEY_ARROW_RIGHT:   return {TMS_KEY_D, TMS_KEY_FUNCTION};

        // --- synthesised: editing keys ----------------------------------------------
        case HID_KEY_BACKSPACE:     return {TMS_KEY_S, TMS_KEY_FUNCTION};   // FCTN+S is the TI's backspace
        case HID_KEY_DELETE:        return {TMS_KEY_1, TMS_KEY_FUNCTION};   // FCTN+1 = DEL
        case HID_KEY_INSERT:        return {TMS_KEY_2, TMS_KEY_FUNCTION};   // FCTN+2 = INS
        case HID_KEY_ESCAPE:        return {TMS_KEY_9, TMS_KEY_FUNCTION};   // FCTN+9 = BACK (quit)

        // --- synthesised: the FCTN number row, on the PC function keys ---------------
        case HID_KEY_F1:            return {TMS_KEY_1, TMS_KEY_FUNCTION};   // DEL
        case HID_KEY_F2:            return {TMS_KEY_2, TMS_KEY_FUNCTION};   // INS
        case HID_KEY_F3:            return {TMS_KEY_3, TMS_KEY_FUNCTION};   // ERASE
        case HID_KEY_F4:            return {TMS_KEY_4, TMS_KEY_FUNCTION};   // CLEAR
        case HID_KEY_F5:            return {TMS_KEY_5, TMS_KEY_FUNCTION};   // BEGIN
        case HID_KEY_F6:            return {TMS_KEY_6, TMS_KEY_FUNCTION};   // PROC'D
        case HID_KEY_F7:            return {TMS_KEY_7, TMS_KEY_FUNCTION};   // AID
        case HID_KEY_F8:            return {TMS_KEY_8, TMS_KEY_FUNCTION};   // REDO
        case HID_KEY_F9:            return {TMS_KEY_9, TMS_KEY_FUNCTION};   // BACK
        case HID_KEY_F10:           return {TMS_KEY_EQUALS, TMS_KEY_FUNCTION}; // QUIT

        // --- synthesised: characters the TI puts elsewhere ---------------------------
        case HID_KEY_MINUS:         return {TMS_KEY_SLASH,  TMS_KEY_SHIFT};    // '-' is SHIFT+/
        case HID_KEY_APOSTROPHE:    return {TMS_KEY_O,      TMS_KEY_FUNCTION}; // '\'' is FCTN+O
        case HID_KEY_BRACKET_LEFT:  return {TMS_KEY_R,      TMS_KEY_FUNCTION}; // '[' is FCTN+R
        case HID_KEY_BRACKET_RIGHT: return {TMS_KEY_T,      TMS_KEY_FUNCTION}; // ']' is FCTN+T
        case HID_KEY_BACKSLASH:     return {TMS_KEY_Z,      TMS_KEY_FUNCTION}; // '\\' is FCTN+Z

        default:                    return {TMS_KEY_NONE, TMS_KEY_NONE};
    }
}

// Alpha Lock is a physical latching key on the TI, so a PC Caps Lock press toggles it
// rather than holding it. Tracked here because HID reports the LED state as a modifier
// only on some keyboards.
static bool alphaLock = false;
static bool capsWasDown = false;

static void update_ti_keyboard(void)
{
    TMS9901_ClearJoyKeyData();

    const auto &kb = io::getCurrentKeyboardState();

    // Modifiers the user is physically holding.
    if (kb.modifier & (KEYBOARD_MODIFIER_LEFTSHIFT | KEYBOARD_MODIFIER_RIGHTSHIFT))
        tms9901.Keyboard[TMS_KEY_SHIFT] = 1;
    if (kb.modifier & (KEYBOARD_MODIFIER_LEFTCTRL | KEYBOARD_MODIFIER_RIGHTCTRL))
        tms9901.Keyboard[TMS_KEY_CONTROL] = 1;
    if (kb.modifier & (KEYBOARD_MODIFIER_LEFTALT | KEYBOARD_MODIFIER_RIGHTALT))
        tms9901.Keyboard[TMS_KEY_FUNCTION] = 1;

    bool capsDown = false;
    for (int i = 0; i < 6; i++)
    {
        uint8_t hid = kb.keycode[i];
        if (!hid) continue;

        if (hid == HID_KEY_CAPS_LOCK) { capsDown = true; continue; }

        TIKeyCombo k = hidKeyToTIKey(hid);
        if (k.key != TMS_KEY_NONE)
        {
            tms9901.Keyboard[k.key] = 1;
            if (k.modifier != TMS_KEY_NONE) tms9901.Keyboard[k.modifier] = 1;
        }
    }

    if (capsDown && !capsWasDown) alphaLock = !alphaLock;
    capsWasDown = capsDown;
    tms9901.CapsLock = alphaLock ? 1 : 0;
}

// -------------------------------------------------------------------------------------
// Joysticks. The TI's two joystick ports are scanned as extra keyboard columns, so they
// land in the same tms9901.Keyboard[] array. USB gamepads take a port each; a GPIO
// NES/SNES pad or a Wii classic controller feeds whichever port has no USB pad.
// -------------------------------------------------------------------------------------
static void update_ti_joysticks(void)
{
    static constexpr int LEFT   = 1 << 6;
    static constexpr int RIGHT  = 1 << 7;
    static constexpr int UP     = 1 << 4;
    static constexpr int DOWN   = 1 << 5;
    static constexpr int SELECT = 1 << 2;
    static constexpr int START  = 1 << 3;
    static constexpr int A      = 1 << 0;
    static constexpr int B      = 1 << 1;

#if NES_PIN_CLK != -1
    // A NES pad shifts its buttons out in NES order already; a SNES pad puts B and Y
    // where A and B would be, so its face buttons are taken by name rather than by
    // position - otherwise physical A does nothing and B fires.
    auto nespadGameBits = [](int padnum) -> int
    {
        if (nespad_padtype[padnum] != NESPAD_TYPE_SNES)
        {
            return nespad_states[padnum];
        }
        const uint16_t ext = nespad_states_ext[padnum];
        int v = ext & (SELECT | START | UP | DOWN | LEFT | RIGHT);
        if (ext & (1u << 8)) v |= A;
        if (ext & (1u << 0)) v |= B;
        return v;
    };
#endif

    static const u8 joyKeys[2][5] = {
        { TMS_KEY_JOY1_UP, TMS_KEY_JOY1_DOWN, TMS_KEY_JOY1_LEFT, TMS_KEY_JOY1_RIGHT, TMS_KEY_JOY1_FIRE },
        { TMS_KEY_JOY2_UP, TMS_KEY_JOY2_DOWN, TMS_KEY_JOY2_LEFT, TMS_KEY_JOY2_RIGHT, TMS_KEY_JOY2_FIRE },
    };

    bool usbConnected = false;

    for (int i = 0; i < 2; ++i)
    {
        auto &gp = io::getCurrentGamePadState(i);
        if (i == 0) usbConnected = gp.isConnected();

        int v = (gp.buttons & io::GamePadState::Button::LEFT   ? LEFT   : 0) |
                (gp.buttons & io::GamePadState::Button::RIGHT  ? RIGHT  : 0) |
                (gp.buttons & io::GamePadState::Button::UP     ? UP     : 0) |
                (gp.buttons & io::GamePadState::Button::DOWN   ? DOWN   : 0) |
                (gp.buttons & io::GamePadState::Button::A      ? A      : 0) |
                (gp.buttons & io::GamePadState::Button::B      ? B      : 0) |
                (gp.buttons & io::GamePadState::Button::SELECT ? SELECT : 0) |
                (gp.buttons & io::GamePadState::Button::START  ? START  : 0);

#if NES_PIN_CLK != -1
        if (usbConnected)
        {
            if (i == 1) v |= nespadGameBits(1) | nespadGameBits(0);
        }
        else
        {
            v |= nespadGameBits(i);
        }
#endif

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        if (usbConnected) { if (i == 1) v |= wiipad_raw_cached; }
        else              { if (i == 0) v |= wiipad_raw_cached; }
#endif

        if (v & UP)            tms9901.Keyboard[joyKeys[i][0]] = 1;
        if (v & DOWN)          tms9901.Keyboard[joyKeys[i][1]] = 1;
        if (v & LEFT)          tms9901.Keyboard[joyKeys[i][2]] = 1;
        if (v & RIGHT)         tms9901.Keyboard[joyKeys[i][3]] = 1;
        if (v & (A | B))       tms9901.Keyboard[joyKeys[i][4]] = 1;

        if (i == 0)
        {
            // Reboot to BOOTSEL mode
            if ((v & (SELECT | START | UP | A)) == (SELECT | START | UP | A))
                reset_usb_boot(0, 0);

            auto pushed = v & ~prevButtons[i];

            if (v & START)
            {
                if (pushed & A)
                {
                    settings.flags.displayFrameRate = !settings.flags.displayFrameRate;
                }
#if HW_CONFIG == 8
                else if (pushed & LEFT)
                {
                    settings.fruitjamVolumeLevel = std::max((int8_t)-63, (int8_t)(settings.fruitjamVolumeLevel - 1));
                    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
                }
                else if (pushed & RIGHT)
                {
                    settings.fruitjamVolumeLevel = std::min((int8_t)23, (int8_t)(settings.fruitjamVolumeLevel + 1));
                    EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);
                }
#endif
            }

            if (v & SELECT)
            {
                if (pushed & START) showSettings = true;
                if (pushed & UP)        scaleMode8_7_ = Frens::screenMode(-1);
                else if (pushed & DOWN) scaleMode8_7_ = Frens::screenMode(+1);
                else if (pushed & LEFT)
                {
#if EXT_AUDIO_IS_ENABLED && !HSTX
                    settings.flags.useExtAudio = !settings.flags.useExtAudio;
#else
                    settings.flags.useExtAudio = 0;
#endif
                }
#if ENABLE_VU_METER
                else if (pushed & RIGHT)
                {
                    settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
                    turnOffAllLeds();
                }
#endif
            }
        }
        prevButtons[i] = v;
    }
}

// Forward declaration - the settings menu's Reset action needs this, and its definition
// sits with the rest of the TI BASIC handling below.
static bool is_tibasic_selection(const char *path);

// =====================================================================================
// Per-frame housekeeping
// =====================================================================================
static void processPerFrame(void)
{
    // Pace to vsync before the next frame is emulated, not after. Scanlines are written
    // straight into the framebuffer as the VDP produces them, so the emulator has to
    // start its frame at the top of vblank - that keeps the writer ahead of the scanout
    // DMA for the whole frame and there is nothing to tear.
    Frens::PaceFrames60fps(false);

    Frens::pollHeadPhoneJack();
    EXT_AUDIO_POLL_HEADPHONE();

#if NES_PIN_CLK != -1
    nespad_read_start();
#endif
    auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
    Frens::blinkLed((count / 60) & 1);
#if NES_PIN_CLK != -1
    nespad_read_finish();
#endif
    tuh_task();

#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_raw_cached = wiipad_read();
#endif

#if ENABLE_VU_METER
    if (isVUMeterToggleButtonPressed())
    {
        settings.flags.enableVUMeter = !settings.flags.enableVUMeter;
        FrensSettings::savesettings();
        turnOffAllLeds();
    }
#endif

    update_ti_keyboard();
    update_ti_joysticks();

    if (showSettings)
    {
        showSettings = false;
        int rval = showSettingsMenu(true);
        if (rval == 3)          // Quit game
        {
            key_done = true;
        }
        if (rval == 5)          // Reset game
        {
            // Reset must go back through the same path the game was started with, or a
            // TI BASIC session would try to load its own marker file as a cartridge.
            char err[64] = {0};
            const char *p = is_tibasic_selection(romName) ? nullptr : romName;
            ti99_load_cart(p, 0, err, sizeof(err));
        }
    }

    // The console has asked for a tape it has not got. SAVE CS1 and OLD CS1 name no
    // file, so this is the only moment the choice can be made - and it is the same
    // moment the console is telling the user to press RECORD or PLAY.
    int tapeReq = cassette_pending_request();
    if (tapeReq)
    {
        cassette_clear_request();
        menuCassettePrompt(tapeReq == TAPE_REQ_RECORD);
    }
}

// =====================================================================================
// TI BASIC marker file
//
// The menu is built around picking a file, but TI BASIC needs no cartridge at all - it
// lives in the console GROMs. A small marker file gives it a menu entry like any other
// title; selecting it boots the console with an empty cartridge slot.
// =====================================================================================
static void ensure_tibasic_marker(void)
{
    static const char *dirs[] = { "/roms/TI99", "/roms/ti99", "/roms" };

    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
    {
        FILINFO fno;
        if (f_stat(dirs[i], &fno) != FR_OK) continue;      // that rom folder does not exist

        char path[FF_MAX_LFN];
        snprintf(path, sizeof(path), "%s/%s", dirs[i], TIBASIC_MARKER);
        if (f_stat(path, &fno) == FR_OK) return;           // already there

        FIL fil;
        if (f_open(&fil, path, FA_WRITE | FA_CREATE_NEW) == FR_OK)
        {
            // Content is never read - selecting the file by its extension is the signal.
            const char *note = "pico-994A: boots the console with no cartridge (TI BASIC)\n";
            UINT bw;
            f_write(&fil, note, strlen(note), &bw);
            f_close(&fil);
            printf("[ti99] created %s\n", path);
        }
        return;
    }
}

static bool is_tibasic_selection(const char *path)
{
    if (!path || !path[0]) return true;
    const char *ext = strrchr(path, '.');
    return (ext && strcasecmp(ext, TIBASIC_EXT) == 0);
}

// =====================================================================================
// main
// =====================================================================================
int main()
{
    char selectedRom[FF_MAX_LFN];
    romName = selectedRom;
    ErrorMessage[0] = selectedRom[0] = 0;

    Frens::setClocksAndStartStdio(CPUFreqKHz, VREG_VOLTAGE_1_20);

    printf("==========================================================================================\n");
    printf("pico-994A (TI-99/4A) %s\n", SWVERSION);
    printf("Build date: %s  time: %s\n", __DATE__, __TIME__);
    printf("CPU freq: %d kHz\n", clock_get_hz(clk_sys) / 1000);
#if HSTX
    printf("HSTX freq: %d\n", clock_get_hz(clk_hstx) / 1000);
#endif
    printf("Stack size: %d bytes\n", PICO_STACK_SIZE);
    printf("==========================================================================================\n");

    FrensSettings::initSettings(FrensSettings::TI99);
    isFatalError = !Frens::initAll(selectedRom, CPUFreqKHz,
                                   TI99_MARGIN_TOP, TI99_MARGIN_BOTTOM,
                                   AUDIOBUFFERSIZE, false, true);

    g_settings_visibility    = g_settings_visibility_ti99;
    g_available_screen_modes = g_available_screen_modes_ti99;
    menuSetCassetteHooks(&cassetteHooks);
    if (!g_available_screen_modes[static_cast<int>(settings.screenMode)])
        settings.screenMode = ScreenMode::NOSCANLINE_1_1;
    scaleMode8_7_ = Frens::applyScreenMode(settings.screenMode);

    // Default machine: NTSC, 32K expansion, plain cartridge, no frame skipping.
    // These fields are *indices* into tables in the core, not values - maxSprites 0
    // selects MaxSprites[0] == 4 (what a real 9918A shows on a line) and spriteCheck 0
    // means "use the default collision scan rate", not "check every 255 lines".
    memset(&myConfig, 0x00, sizeof(myConfig));
    myConfig.machineType = MACH_TYPE_NORMAL32K;
    myConfig.cartType    = CART_TYPE_NORMAL;
    myConfig.maxSprites  = 0;          // 0 = hardware accurate (4/line), 1 = 32, no flicker
    myConfig.spriteCheck = 0;          // 0 = default scan rate
    myConfig.frameSkip   = 0;          // 0 = never skip
    myConfig.isPAL       = 0;

    ensure_tibasic_marker();

    bool showSplash = true;

    while (true)
    {
        if (strlen(selectedRom) == 0)
        {
            // .rpk is the format to prefer - one file holding every part. The classic
            // C/D/G sets are .bin, and .tib is the marker that boots TI BASIC.
            // RomLister splits this list on spaces (see RomLister::IsextensionAllowed),
            // not commas, and compares case-insensitively.
            const char *romExtensions = ".rpk .bin .tib";
            menu("pico-994A", ErrorMessage, isFatalError, showSplash, romExtensions, selectedRom);
            printf("Selected: %s\n", selectedRom);
        }

        *ErrorMessage = 0;
        EXT_AUDIO_SETVOLUME(settings.fruitjamVolumeLevel);

        ti99_free_memory();
        if (ti99_alloc_memory() != 0)
        {
            strcpy(ErrorMessage, "Out of memory");
            printf("%s\n", ErrorMessage);
            selectedRom[0] = 0;
            continue;
        }
        Frens::dumpHeapStats("after ti99_alloc_memory");

        if (ti99_load_bios(ErrorMessage, 40) != 0)
        {
            printf("%s\n", ErrorMessage);
            ti99_free_memory();
            selectedRom[0] = 0;
            continue;
        }

        const char *cartPath = is_tibasic_selection(selectedRom) ? nullptr : selectedRom;
        if (ti99_load_cart(cartPath, 1, ErrorMessage, 40) != 0)
        {
            printf("%s\n", ErrorMessage);
            ti99_free_memory();
            selectedRom[0] = 0;
            continue;
        }
        Frens::dumpHeapStats("after cart load");

        build_palette();
        ti99_psg_init(TI99_AUDIO_SAMPLE_RATE);

        if (showSplash && !Frens::isPsramEnabled())
        {
            showSplash = false;
            menuPumpBlankFrames(180);
        }

        Frens::PaceFrames60fps(true);
        start_tick_us = Frens::time_us();
        prevButtons[0] = prevButtons[1] = 0;
        key_done = false;

        // Emulation loop: run scanlines until the VDP says the frame is done, then
        // hand off audio and let processPerFrame pace us to the display.
        while (!key_done)
        {
            processPerFrame();                  // pace to vsync, then run the frame
            while (LoopTMS9900()) { }           // one frame of CPU + VDP, rendering as it goes
            cassette_frame_tick();              // tape prefetch / flush - the only SD access
            process_audio_frame();
            SpeechTraceTick();                  // no-op unless -DTI99_SPEECH_TRACE
        }

        // Back to the menu: release the machine so RomLister and the artwork loader
        // have the heap to themselves again.
        ti99_free_memory();
        Frens::dumpHeapStats("after ti99_free_memory");

        selectedRom[0] = 0;
        showSplash = false;
    }

    return 0;
}
