// See disk.c for info 

#include "ti99.h"

#define MAX_DSK_SIZE        (360*1024)  // 360K maximum .dsk size
#define MAX_DSK_SECTORS     1440        // For all disks we support 1440x256=360K

// Three disks supported.. that should be fine for just about anything
enum
{
    DSK1=0,
    DSK2,
    DSK3,
    MAX_DSKS
};

// -------------------------------------------------------------------------------------
// Upstream keeps whole 360K images in RAM (two of them, plus a third on DSi) and writes
// dirty sectors back on unmount. That is 720K+ - more than the RP2350 has in total - so
// this port does sector-level I/O straight against the .DSK file instead, through a
// handle held open for as long as the disk is mounted. Writes go through immediately,
// which also means a power cut mid-session cannot lose a saved program.
// -------------------------------------------------------------------------------------
typedef struct
{
    u8   isMounted;                     // Is this disk mounted?
    u8   isReadOnly;                    // Opened read-only (file could not be opened for write)
    u8   driveReadCounter;              // Set to some non-zero value to show 'DISK READ' briefly on screen
    u8   driveWriteCounter;             // Set to some non-zero value to show 'DISK WRITE' briefly on screen (takes priority over READ display)
    char filename[MAX_PATH];            // The name of the .dsk file
    char path[MAX_PATH];                // The directory where the .dsk file was found
    char fullpath[MAX_PATH];            // path + '/' + filename, so no chdir() is needed
    void *fp;                           // Open TI99_FILE handle while mounted, else NULL
}  Disk_t;

extern Disk_t Disk[MAX_DSKS];

#define MAX_FILES_PER_DSK           38          // We allow 38 files shown per disk... that's enough for our purposes and it's what we can show on screen comfortably
#define MAX_DSK_FILE_LEN            12          // And room for the filename (really 10 max chars plus NULL... keep on even byte boundary)

typedef struct
{
    char    filename[MAX_DSK_FILE_LEN]; // TI Filename (10 chars max)
    u16     filesize;                   // TI file size (in sectors)
} DiskList_t;

extern DiskList_t dsk_listing[MAX_FILES_PER_DSK];   // We store the disk listing here...
extern u8 dsk_num_files;                            // And we found this many TI files...

extern u8 TICC_REG[8];
extern u8 TICC_DIR;
extern u8 bDiskDeviceInstalled;
extern u8 diskSideSelected;
extern u8 driveSelected;
extern u8 motorOn;

extern void disk_init(void);
extern u8   ReadTICCRegister(u16 address);
extern void WriteTICCRegister(u16 address, u8 val);
extern void HandleTICCSector(void);
extern void disk_cru_write(u16 address, u8 data);
extern u8   disk_cru_read(u16 address);
extern void disk_mount(u8 drive, char *path, char *filename);
extern void disk_unmount(u8 drive);
extern void disk_read_from_sd(u8 drive);
extern void disk_write_to_sd(u8 disk);   // now just an fsync - writes are immediate
extern void disk_backup_to_sd(u8 disk);
extern void disk_get_file_listing(u8 drive);
extern u16  disk_get_used_sectors(u8 drive, u16 numSectors);
extern void disk_create_blank(void);

// End of file
