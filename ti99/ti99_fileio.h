// =====================================================================================
// ti99_fileio.h - <stdio.h> / <fat.h> file access on top of FatFS.
//
// The DS994a sources use libfat, which gives them a POSIX stdio interface backed by
// the DS card slot. pico_shared has no such shim - it exposes FatFS directly - so
// this maps the handful of calls the core actually makes onto f_open/f_read/...
//
// Only the calls the ported core uses are implemented; this is deliberately not a
// general purpose stdio. FIL is ~560 bytes with FF_FS_TINY==0, so handles are heap
// allocated rather than placed on the stack (the menu and emulator run close to the
// stack limit already).
// =====================================================================================
#ifndef _TI99_FILEIO_H_
#define _TI99_FILEIO_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TI99_FILE TI99_FILE;

TI99_FILE *ti99_fopen(const char *path, const char *mode);
int        ti99_fclose(TI99_FILE *f);
size_t     ti99_fread(void *ptr, size_t size, size_t nmemb, TI99_FILE *f);
size_t     ti99_fwrite(const void *ptr, size_t size, size_t nmemb, TI99_FILE *f);
int        ti99_fseek(TI99_FILE *f, long offset, int whence);
long       ti99_ftell(TI99_FILE *f);
int        ti99_fflush(TI99_FILE *f);

int        ti99_remove(const char *path);
int        ti99_mkdir(const char *path);          // 0 on success or already present
int        ti99_file_exists(const char *path);
long       ti99_file_size(const char *path);      // -1 if absent

// Redirect the upstream call names. Include this header after <stdio.h>.
// ti99_fileio.c defines TI99_FILEIO_NO_REDIRECT so it can see the real FatFS API.
#ifndef TI99_FILEIO_NO_REDIRECT
#define FILE    TI99_FILE
#define fopen   ti99_fopen
#define fclose  ti99_fclose
#define fread   ti99_fread
#define fwrite  ti99_fwrite
#define fseek   ti99_fseek
#define ftell   ti99_ftell
#define fflush  ti99_fflush
#define remove  ti99_remove
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#ifdef __cplusplus
}
#endif

#endif // _TI99_FILEIO_H_
