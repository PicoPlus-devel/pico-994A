// Host test harness for pico-994A cassette support.
//
// Compiles the REAL ti99/cassette.c and ti99/cpu/tms9900/tms9901.c with stubs standing
// in for the CPU core and FatFs, so the tests drive the same code the firmware runs -
// CRU writes and reads go through TMS9901_WriteCRU / TMS9901_ReadCRU exactly as the
// TMS9900 would issue them.
#define TI99_FILEIO_NO_REDIRECT 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#undef DIR_HOST

#include "ti99_compat.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9900/tms9901.h"
#include "cassette.h"
#include "ff.h"

// =====================================================================================
// CPU / peripheral stubs
// =====================================================================================
TMS9900 tms9900;

u8  MemCPU_stub[4];
u8 *MemCPU  = MemCPU_stub;
u8 *MemGROM = MemCPU_stub;
u8 *MemCART = MemCPU_stub;
u8 *pVDPVidMem = MemCPU_stub;
u8 *DISK_DSR = MemCPU_stub;
u8 *SharedMemBuffer = MemCPU_stub;
u8  fileBuf[0x2000];
char tmpBuf[256];
u32 file_size;
u32 MAX_CART_SIZE;

int timerInterruptCount = 0;

void TMS9900_ClearInterrupt(u16 m) { (void)m; }
void TMS9900_RaiseInterrupt(u16 m) { (void)m; }
void TMS9900_SetAccurateEmulationFlag(u16 f) { (void)f; }
void TMS9900_ClearAccurateEmulationFlag(u16 f) { (void)f; }

void disk_cru_write(u16 a, u8 d)  { (void)a; (void)d; }
u8   disk_cru_read(u16 a)         { (void)a; return 1; }
void SAMS_cru_write(u16 a, u8 d)  { (void)a; (void)d; }
u8   SAMS_cru_read(u16 a)         { (void)a; return 1; }
void pcode_cru_write(u16 a, u8 d) { (void)a; (void)d; }
void cart_cru_write(u16 a, u8 d)  { (void)a; (void)d; }
u8   cart_cru_read(u16 a)         { (void)a; return 1; }

// =====================================================================================
// File I/O: map the emulator's tape directory onto ./sandbox
// =====================================================================================
#define SANDBOX "sandbox"

static const char *remap(const char *path, char *out, size_t n)
{
    // "/saves/ti99/tapes/FOO.wav" -> "sandbox/FOO.wav"; anything else keeps its tail.
    const char *slash = strrchr(path, '/');
    snprintf(out, n, SANDBOX "/%s", slash ? slash + 1 : path);
    return out;
}

typedef struct TI99_FILE { FILE *f; } TI99_FILE;

TI99_FILE *ti99_fopen(const char *path, const char *mode)
{
    char p[512];
    FILE *f = fopen(remap(path, p, sizeof(p)), mode);
    if (!f) return NULL;
    TI99_FILE *h = malloc(sizeof(TI99_FILE));
    h->f = f;
    return h;
}
int    ti99_fclose(TI99_FILE *h) { int r = fclose(h->f); free(h); return r; }
size_t ti99_fread(void *p, size_t s, size_t n, TI99_FILE *h)  { return fread(p, s, n, h->f); }
size_t ti99_fwrite(const void *p, size_t s, size_t n, TI99_FILE *h) { return fwrite(p, s, n, h->f); }
int    ti99_fseek(TI99_FILE *h, long o, int w) { return fseek(h->f, o, w); }
long   ti99_ftell(TI99_FILE *h) { return ftell(h->f); }
int    ti99_fflush(TI99_FILE *h) { return fflush(h->f); }
int    ti99_remove(const char *path) { char p[512]; return remove(remap(path, p, sizeof(p))); }
int    ti99_mkdir(const char *path) { (void)path; mkdir(SANDBOX, 0777); return 0; }

int ti99_file_exists(const char *path)
{
    char p[512];
    struct stat st;
    return (stat(remap(path, p, sizeof(p)), &st) == 0);
}
long ti99_file_size(const char *path)
{
    char p[512];
    struct stat st;
    if (stat(remap(path, p, sizeof(p)), &st) != 0) return -1;
    return (long)st.st_size;
}

// Directory enumeration over the sandbox.
static struct dirent **g_ents;
static int g_nents, g_ient;

int f_opendir(DIR *d, const char *p)
{
    (void)d; (void)p;
    g_nents = scandir(SANDBOX, &g_ents, NULL, alphasort);
    g_ient = 0;
    return (g_nents < 0) ? 1 : FR_OK;
}
int f_readdir(DIR *d, FILINFO *fi)
{
    (void)d;
    while (g_ient < g_nents)
    {
        const char *n = g_ents[g_ient++]->d_name;
        if (n[0] == '.') continue;
        snprintf(fi->fname, sizeof(fi->fname), "%s", n);
        fi->fattrib = 0;
        fi->fsize = 0;
        return FR_OK;
    }
    fi->fname[0] = 0;
    return FR_OK;
}
int f_closedir(DIR *d) { (void)d; return FR_OK; }
