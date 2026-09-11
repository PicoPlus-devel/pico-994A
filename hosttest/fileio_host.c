// =====================================================================================
// fileio_host.c - host stand-in for ti99/ti99_fileio.c, which sits on FatFs.
//
// /bios/NAME is looked up in the repository's bios/ directory (BIOS_DIR, set by
// build.sh), ignoring case: the files there are lower case while the core asks for
// 994aROM.bin. The other places the core looks (/roms/...) are made to miss, so nothing
// on the host is picked up by accident. Read-only: nothing is written, created or removed.
// =====================================================================================
#define TI99_FILEIO_NO_REDIRECT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ti99_fileio.h"

struct TI99_FILE { FILE *f; };

static void map_path(const char *in, char *out, size_t n)
{
    if (strncmp(in, "/bios/", 6) == 0)
    {
        const char *name = in + 6;
        snprintf(out, n, "%s/%s", BIOS_DIR, name);
        DIR *d = opendir(BIOS_DIR);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (strcasecmp(e->d_name, name) == 0) { snprintf(out, n, "%s/%s", BIOS_DIR, e->d_name); break; }
        if (d) closedir(d);
        return;
    }
    if (strncmp(in, "/roms/", 6) == 0) { snprintf(out, n, "/nonexistent%s", in); return; }
    snprintf(out, n, "%s", in);
}

TI99_FILE *ti99_fopen(const char *path, const char *mode)
{
    char p[1024];
    map_path(path, p, sizeof p);
    if (strpbrk(mode, "wa+")) return NULL;
    FILE *f = fopen(p, mode);
    if (!f) return NULL;
    TI99_FILE *t = malloc(sizeof *t);
    t->f = f;
    return t;
}

int    ti99_fclose(TI99_FILE *f)                                 { if (!f) return -1; fclose(f->f); free(f); return 0; }
size_t ti99_fread(void *p, size_t s, size_t n, TI99_FILE *f)      { return fread(p, s, n, f->f); }
size_t ti99_fwrite(const void *p, size_t s, size_t n, TI99_FILE *f) { (void)p; (void)s; (void)n; (void)f; return 0; }
int    ti99_fseek(TI99_FILE *f, long o, int w)                   { return fseek(f->f, o, w); }
long   ti99_ftell(TI99_FILE *f)                                  { return ftell(f->f); }
int    ti99_fflush(TI99_FILE *f)                                 { (void)f; return 0; }
int    ti99_remove(const char *path)                             { (void)path; return -1; }
int    ti99_mkdir(const char *path)                              { (void)path; return 0; }

int ti99_file_exists(const char *path)
{
    char p[1024];
    struct stat st;
    map_path(path, p, sizeof p);
    return stat(p, &st) == 0;
}

long ti99_file_size(const char *path)
{
    char p[1024];
    struct stat st;
    map_path(path, p, sizeof p);
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}
