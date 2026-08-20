// =====================================================================================
// ti99_fileio.c - see ti99_fileio.h
// =====================================================================================
#define TI99_FILEIO_NO_REDIRECT 1

#include <stdlib.h>
#include <string.h>
#include "ff.h"
#include "ti99_fileio.h"

struct TI99_FILE
{
    FIL fil;
    int writable;
};

TI99_FILE *ti99_fopen(const char *path, const char *mode)
{
    if (!path || !mode) return NULL;

    BYTE flags = 0;
    int writable = 0;

    // The core only ever asks for "rb", "wb" and "rb+".
    if (mode[0] == 'r')
    {
        flags = FA_READ;
        if (strchr(mode, '+')) { flags |= FA_WRITE; writable = 1; }
    }
    else if (mode[0] == 'w')
    {
        flags = FA_WRITE | FA_CREATE_ALWAYS;
        if (strchr(mode, '+')) flags |= FA_READ;
        writable = 1;
    }
    else if (mode[0] == 'a')
    {
        flags = FA_WRITE | FA_OPEN_APPEND;
        if (strchr(mode, '+')) flags |= FA_READ;
        writable = 1;
    }
    else
    {
        return NULL;
    }

    TI99_FILE *f = (TI99_FILE *)malloc(sizeof(TI99_FILE));
    if (!f) return NULL;

    if (f_open(&f->fil, path, flags) != FR_OK)
    {
        free(f);
        return NULL;
    }
    f->writable = writable;
    return f;
}

int ti99_fclose(TI99_FILE *f)
{
    if (!f) return -1;
    FRESULT fr = f_close(&f->fil);
    free(f);
    return (fr == FR_OK) ? 0 : -1;
}

size_t ti99_fread(void *ptr, size_t size, size_t nmemb, TI99_FILE *f)
{
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    UINT br = 0;
    if (f_read(&f->fil, ptr, (UINT)(size * nmemb), &br) != FR_OK) return 0;
    return br / size;   // stdio returns whole items read
}

size_t ti99_fwrite(const void *ptr, size_t size, size_t nmemb, TI99_FILE *f)
{
    if (!f || !ptr || size == 0 || nmemb == 0) return 0;
    UINT bw = 0;
    if (f_write(&f->fil, ptr, (UINT)(size * nmemb), &bw) != FR_OK) return 0;
    return bw / size;
}

int ti99_fseek(TI99_FILE *f, long offset, int whence)
{
    if (!f) return -1;
    FSIZE_t target;
    switch (whence)
    {
        case SEEK_SET: target = (FSIZE_t)offset;                        break;
        case SEEK_CUR: target = f_tell(&f->fil) + (FSIZE_t)offset;      break;
        case SEEK_END: target = f_size(&f->fil) + (FSIZE_t)offset;      break;
        default:       return -1;
    }
    return (f_lseek(&f->fil, target) == FR_OK) ? 0 : -1;
}

long ti99_ftell(TI99_FILE *f)
{
    if (!f) return -1;
    return (long)f_tell(&f->fil);
}

int ti99_fflush(TI99_FILE *f)
{
    if (!f) return -1;
    if (!f->writable) return 0;
    return (f_sync(&f->fil) == FR_OK) ? 0 : -1;
}

int ti99_remove(const char *path)
{
    return (f_unlink(path) == FR_OK) ? 0 : -1;
}

int ti99_mkdir(const char *path)
{
    FRESULT fr = f_mkdir(path);
    return (fr == FR_OK || fr == FR_EXIST) ? 0 : -1;
}

int ti99_file_exists(const char *path)
{
    FILINFO fno;
    return (f_stat(path, &fno) == FR_OK) ? 1 : 0;
}

long ti99_file_size(const char *path)
{
    FILINFO fno;
    if (f_stat(path, &fno) != FR_OK) return -1;
    return (long)fno.fsize;
}
