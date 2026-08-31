// Minimal FatFs stand-in, just enough for the host test of cassette.c.
#ifndef FF_H
#define FF_H
#include <stdint.h>
#define FR_OK 0
#define AM_DIR 0x10
#define AM_HID 0x02
#define AM_SYS 0x04
typedef struct { int dummy; } FF_DIR;
#define DIR FF_DIR
typedef struct { unsigned long fsize; unsigned char fattrib; char fname[256]; } FILINFO;
int f_opendir(DIR *d, const char *p);
int f_readdir(DIR *d, FILINFO *f);
int f_closedir(DIR *d);
#endif
