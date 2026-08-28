#include "menu.h"
#include "FrensHelpers.h"
#include <cstring>

static int fgcolorSplash = DEFAULT_FGCOLOR;
static int bgcolorSplash = DEFAULT_BGCOLOR;

void splash()
{
    char s[SCREEN_COLS + 1];
    ClearScreen(bgcolorSplash);

    strcpy(s, "pico-994A");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 2, s, fgcolorSplash, bgcolorSplash);

    strcpy(s, "TI-99/4A for RP2350");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 3, s, fgcolorSplash, bgcolorSplash);

    strcpy(s, "TI-99/4A Emulation Core");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 5, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "DS994a by wavemotion-dave");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 6, s, CLIGHTBLUE, bgcolorSplash);
    strcpy(s, "TMS9900 Classic99 (Tursi)");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 7, s, CLIGHTBLUE, bgcolorSplash);
    strcpy(s, "TMS9918A ColEM (M.Fayzullin)");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 8, s, CLIGHTBLUE, bgcolorSplash);

#if !HSTX
    strcpy(s, "Pico DVI");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 10, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "@shuichi_takano");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 11, s, CLIGHTBLUE, bgcolorSplash);
#else
    strcpy(s, "Pico DVI____________HDMI Driver");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 10, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "@shuichi_takano_____fliperama86");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 11, s, CLIGHTBLUE, bgcolorSplash);
#endif

    strcpy(s, "Pico Port & SD Card Support");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 14, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "@frenskefrens");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 15, s, CLIGHTBLUE, bgcolorSplash);

    strcpy(s, "NES/WII controller support");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 18, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "@PaintYourDragon @adafruit");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 19, s, CLIGHTBLUE, bgcolorSplash);

    strcpy(s, "Needs 994aROM.bin + 994aGROM.bin");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 22, s, fgcolorSplash, bgcolorSplash);
    strcpy(s, "in /bios on the SD card");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 23, s, CLIGHTBLUE, bgcolorSplash);

    strcpy(s, "https://github.com/");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 25, s, CLIGHTBLUE, bgcolorSplash);
    strcpy(s, "PicoPlus-devel/pico-994A");
    putText(SCREEN_COLS / 2 - strlen(s) / 2, 26, s, CLIGHTBLUE, bgcolorSplash);
}
