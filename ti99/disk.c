// Updated by wavemotion-dave in 2023, 2024 and 2025.
//
// The original version of this came from Clasic99 (C) Mike Brent
// who has graciously allowed me to use a bit of this code for
// very simplified DSK support. We utilize the TI99 Disk Controller
// DSR along with support for simple 90K, 180K and 360K raw sector disks.
// This should be enough to load up Scott Adam's Adventure Games
// and Tunnels of Doom quests.  Despite the heavy similification and
// porting of the code to the DS, Mike's original copyright remains below:
//
//
// (C) 2013 Mike Brent aka Tursi aka HarmlessLion.com
// This software is provided AS-IS. No warranty
// express or implied is provided.
//
// This notice defines the entire license for this code.
// All rights not explicity granted here are reserved by the
// author.
//
// You may redistribute this software provided the original
// archive is UNCHANGED and a link back to my web page,
// http://harmlesslion.com, is provided as the author's site.
// It is acceptable to link directly to a subpage at harmlesslion.com
// provided that page offers a URL for that purpose
//
// Source code, if available, is provided for educational purposes
// only. You are welcome to read it, learn from it, mock
// it, and hack it up - for your own use only.
//
// Please contact me before distributing derived works or
// ports so that we may work out terms. I don't mind people
// using my code but it's been outright stolen before. In all
// cases the code must maintain credit to the original author(s).
//
// -COMMERCIAL USE- Contact me first. I didn't make
// any money off it - why should you? ;) If you just learned
// something from this, then go ahead. If you just pinched
// a routine or two, let me know, I'll probably just ask
// for credit. If you want to derive a commercial tool
// or use large portions, we need to talk. ;)
//
// If this, itself, is a derived work from someone else's code,
// then their original copyrights and licenses are left intact
// and in full force.
//
// http://harmlesslion.com - visit the web page for contact info

#include "ti99_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ti99_fileio.h"
#include "ti99.h"
#include "cpu/tms9900/tms9901.h"
#include "cpu/tms9900/tms9900.h"
#include "cpu/tms9918a/tms9918a.h"
#include "disk.h"

u8 TICC_REG[8] = {0,0,0,0,0,0,0,0};
u8 TICC_DIR=0;   // 0 means towards track 0

u8 bDiskDeviceInstalled  = 0;       // DSR installed or not installed... We don't do much with this yet.
u8 diskSideSelected      = 0;       // Side 0 or Side 1
u8 driveSelected         = DSK1;    // We support DSK1, DSK2 and DSK3
u8 motorOn               = 0;       // 1=Motor On (Enabled/Strobed)

Disk_t Disk[MAX_DSKS];              // Per-drive metadata plus the open file handle

// Sector 0 of the mounted disk, cached per drive. The DSR reads the volume header
// (sector count, name) and the file-descriptor index (sector 1) far more often than
// any data sector, and both are needed to render a listing; caching sector 0 keeps
// the common case off the SD card. 256 bytes x 3 drives.
static u8 sector0_cache[MAX_DSKS][256];
static u8 sector0_valid[MAX_DSKS];

#define ERR_DEVICEERROR     6       // This is the only error we support. Good enough.

// ------------------------------------------------------
// Start with no disk mounted and all image data clear
// ------------------------------------------------------
void disk_init(void)
{
    for (u8 d = 0; d < MAX_DSKS; d++)
    {
        if (Disk[d].fp) fclose((FILE *)Disk[d].fp);
    }
    memset(Disk, 0x00, sizeof(Disk));
    memset(sector0_valid, 0x00, sizeof(sector0_valid));

    bDiskDeviceInstalled  = 0;
    diskSideSelected      = 0;
    driveSelected         = DSK1;
    motorOn               = 0;
}

// ------------------------------------------------------
// I don't know if anyone will read the disk CRU bits
// but we do our best to fill in the required data.
// ------------------------------------------------------
u8 disk_cru_read(u16 address)
{
    switch (address & 0x07)
    {
        case 0:     return 0;                                   // Is Head Load Requested
        case 1:     return ((driveSelected == DSK1) ? 1:0);     // Is drive DSK1 selected
        case 2:     return ((driveSelected == DSK2) ? 1:0);     // Is drive DSK2 selected
        case 3:     return ((driveSelected == DSK3) ? 1:0);     // Is drive DSK3 selected
        case 4:     return !motorOn;                            // Is Motor On/Enabled/Strobed (inverted)
        case 5:     return 0;                                   // Always 0
        case 6:     return 1;                                   // Always 1
        case 7:     return (diskSideSelected ? 1:0);            // Disk Side Selected (1 or 0)
    }
    return 0;
}

// ------------------------------------------------------
// The CRU write selects which drive (DSK1, DSK2 or DSK3)
// is currently selected and, quite importantly, also
// maps the disk DSR into memory or out of memory.
// ------------------------------------------------------
void disk_cru_write(u16 address, u8 data)
{
    switch (address & 0x07)
    {
        case 0:
            // DISK_DSR is NULL when 994aDISK.bin was not on the card, in which case
            // there is no controller to switch in and the CRU write is a no-op - the
            // same thing the machine does with no disk card fitted.
            bDiskDeviceInstalled = (DISK_DSR != NULL) ? data : 0;
            if (data && DISK_DSR)
            {
                // The Disk Controller DSR is visible
                memcpy(&MemCPU[0x4000], DISK_DSR, 0x2000);
                MemType[0x5ff0>>4] = MF_DISK;     // Disk Control registers ARE visible
            }
            else
            {
                // The Disk Controller DSR is not visible
                memset(&MemCPU[0x4000], 0xFF, 0x2000);
                MemType[0x5ff0>>4] = MF_PERIF;     // Disk Control registers NOT visible
            }
            break;
        case 1:
            motorOn = data;  // If enabled, strobe motor for 4.23 seconds... we just track motor on/off
            break;
        case 4:     // select drive 1
        case 5:     // select drive 2
        case 6:     // select drive 3
            driveSelected = DSK1 + (address & 0x03);    // This will work out to DSK1, DSK2 or DSK3
            break;

        case 7:
            diskSideSelected = (data ? 1:0);
            break;

        default:
            break;
    }
}

u8 ReadTICCRegister(u16 address)
{
    if (address >= 0x5ff8) return 0xFF;

    switch (address & 0xFFFE)
    {
    case 0x5ff0:
        // status register
        TICC_REG[0] = 0x20; // head loaded, ready, not busy, not track 0, no index pulse, etc
        if (TICC_REG[1]==0)
        {
            TICC_REG[0]|=0x04;
        }
        TICC_REG[0]=~TICC_REG[0];
        return TICC_REG[0];

    case 0x5ff2:
        // track register
        return TICC_REG[1];

    case 0x5ff4:
        // sector register
        return TICC_REG[2];

    case 0x5ff6:
        // data register
        return TICC_REG[3];
    }

    return 0x00;
}

void WriteTICCRegister(u16 address, u8 val)
{
    if ((address < 0x5ff8) || (address > 0x5fff)) return;

    switch (address&0xfffe)
    {
    case 0x5ff8:
        // command register
        switch (val & 0xe0)
        {
        case 0x00:
            // restore or seek
            if (val&0x10) {
                // seek to data reg
                TICC_REG[1]=TICC_REG[3];
            } else {
                // seek to track 0
                TICC_REG[1]=0;
            }
            break;

        case 0x20:
            // step
            if (val&0x10)   {   // if update track register
                if (TICC_DIR) {
                    if (TICC_REG[1] < 255) TICC_REG[1]++;
                } else {
                    if (TICC_REG[1] > 0) TICC_REG[1]--;
                }
            }
            break;

        case 0x40:
            // step in
            if (val&0x10)   {   // if update track register
                if (TICC_REG[1] < 255) TICC_REG[1]++;
            }
            break;

        case 0x60:
            // step out
            if (val&0x10)   {   // if update track register
                if (TICC_REG[1] > 0) TICC_REG[1]--;
            }
            break;
        }
        break;

    case 0x5ffA:
        // track register
        TICC_REG[1] = val;
        break;

    case 0x5ffC:
        // sector register
        TICC_REG[2] = val;
        break;

    case 0x5ffE:
        // data register
        TICC_REG[3] = val;
        break;
    }
}

// --------------------------------------------------------------------------
// This is really only used for DSK3 on the older DS-Lite/Phat where the
// disk is not fully buffered in memory and so we must go out to the actual
// .dsk file and seek/read in the sector.
// --------------------------------------------------------------------------
static void ReadSector(u8 drive, u16 sector, u8 *buf)
{
    u8 error = true; // Until proven otherwise...

    if ((sector < MAX_DSK_SECTORS) && Disk[drive].fp)
    {
        // Sector 0 (the volume header) is read constantly - serve it from cache.
        if ((sector == 0) && sector0_valid[drive])
        {
            memcpy(buf, sector0_cache[drive], 256);
            return;
        }

        FILE *fp = (FILE *)Disk[drive].fp;
        if (fseek(fp, (long)(256 * sector), SEEK_SET) == 0)
        {
            if (fread(buf, 1, 256, fp) == 256) error = false;
        }

        if (!error && (sector == 0))
        {
            memcpy(sector0_cache[drive], buf, 256);
            sector0_valid[drive] = 1;
        }
    }

    // If we had any error, clear the buffer
    if (error)
    {
        memset(buf, 0x00, 256); // Just return zeros... good enough on failure
    }
}

// --------------------------------------------------------------------------
// Write one 256 byte sector straight through to the .DSK file. Returns true on
// success. f_sync after each write costs a FAT update but means a program the
// user just SAVEd survives yanking the power, which is the whole point of
// having disk support on a machine with no shutdown step.
// --------------------------------------------------------------------------
static bool WriteSector(u8 drive, u16 sector, u8 *buf)
{
    if (sector >= MAX_DSK_SECTORS) return false;
    if (!Disk[drive].fp || Disk[drive].isReadOnly) return false;

    FILE *fp = (FILE *)Disk[drive].fp;
    if (fseek(fp, (long)(256 * sector), SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, 256, fp) != 256) return false;
    fflush(fp);

    if (sector == 0) sector0_valid[drive] = 0;   // header changed - drop the cache
    return true;
}

// ----------------------------------------------------------------------------------------------------------------
// This is where the magic happens.. this ruotine is called to handle a sector and is done cleverly by way of
// looking at when the PC counter is at >40E8 whcih is the TI disk controller DSR's entry to handle sector
// reads and writes. In this way we can utilize the existing TI disk controller DSR and just handle the actual
// sector read/write. This DSR allows us up to the standard 1600 bits x 256 sectors or 400K of disk space. We
// limit to 360K which is the sort of standard Double-Sided Double-Density drive. If we wanted to go beyond
// this limit we would switch to a different disk controller DSR or create our own. This shouldn't be much of a
// problem as virtually anything that has come out on disk for the TI99 will run on a 360K or smaller floppy.
// ----------------------------------------------------------------------------------------------------------------
 void HandleTICCSector(void)
{
    bool success = true;
    extern u8 *pVDPVidMem;

    if (!bDiskDeviceInstalled) return; // We hit the correct PC counter but the DSR wasn't swapped in so ignore it...

    if (driveSelected != 1 && driveSelected != 2  && driveSelected != 3) // We only support DSK1, DSK2 or DSK3
    {
        MemCPU[0x8350] = ERR_DEVICEERROR;
        tms9900.PC = 0x42a0;                // error 31 (not found)
    }

    // 834A = sector number
    // 834C = drive (1-3)
    // 834D = 0: write, anything else = read
    // 834E = VDP buffer address
    u8  drive        = MemCPU[0x834C];
    u8  isRead       = MemCPU[0x834D];
    u16 sectorNumber = (MemCPU[0x834A]<<8) | MemCPU[0x834B];
    u16 destVDP      = (MemCPU[0x834E]<<8) | MemCPU[0x834F];

    // The VDP address bus is 14 bits, so the transfer window has to be masked into the
    // 16K of video RAM. Upstream gets away without this because its pVDPVidMem sits in
    // a 4MB address space where an overrun is harmless; here it is a 16K heap block and
    // a sector transfer near the top would write over whatever follows it.
    destVDP &= 0x3FFF;
    if (destVDP > (0x4000 - 256)) destVDP = 0x4000 - 256;

    if ((drive == 1) || (drive == 2) || (drive == 3))
    {
        drive = drive-1;    // Zero based for struct array lookup

        // --------------------------------------------------
        // Make sure the sector asked for is within reason...
        // --------------------------------------------------
        if (sectorNumber >=  MAX_DSK_SECTORS)
        {
            success = false;
        }
        else
        {
            if (isRead)
            {
                // -----------------------------------------------------------------
                // Move the 256 byte sector from the .DSK file into VDP memory
                // -----------------------------------------------------------------
                ReadSector(drive, sectorNumber, &pVDPVidMem[destVDP]);
                *((u16*)&MemCPU[0x834A]) = sectorNumber;     // fill in the return data
                Disk[drive].driveReadCounter = 2;            // briefly show that we are reading from the disk
                MemCPU[0x8350] = 0;                          // should still be 0 if no error occurred
            }
            else  // Must be write
            {
                if (WriteSector(drive, sectorNumber, &pVDPVidMem[destVDP]))
                {
                    *((u16*)&MemCPU[0x834A]) = sectorNumber;     // fill in the return data
                    Disk[drive].driveWriteCounter = 2;           // And briefly show that we are writing to the disk
                    MemCPU[0x8350] = 0;                          // should still be 0 if no error occurred
                }
                else
                {
                    success = false;    // Not mounted, read-only media, or an SD error
                }
            }
        }
    } else success = false;


    if (success)
    {
        tms9900.PC = 0x4676;        // return from read or write (write normally goes through read to verify)
    }
    else
    {
        MemCPU[0x8350] = ERR_DEVICEERROR;
        tms9900.PC = 0x42a0;                // error 31 (not found)
    }
}


// --------------------------------------------------------------------------------------------------
// Routines below this comment are all related to reading and writing .DSK files to and from the
// DS Fat file system on the SD Card. We take care to handle .BAK files in case we run into any
// problems with writing the .DSK back to the SD card. Better safe than sorry.
// --------------------------------------------------------------------------------------------------

void disk_mount(u8 drive, char *path, char *filename)
{
    if (drive >= MAX_DSKS) return;

    disk_unmount(drive);    // Drop any handle already held for this drive

    strcpy(Disk[drive].path, path);
    strcpy(Disk[drive].filename, filename);

    // Upstream chdir()s into the disk directory and opens a relative name. FatFS has
    // a single current directory shared with the menu and the cart loader, so build
    // an absolute path once instead and leave the working directory alone.
    if (path[0] && strcmp(path, "/") != 0)
        snprintf(Disk[drive].fullpath, MAX_PATH, "%s/%s", path, filename);
    else
        snprintf(Disk[drive].fullpath, MAX_PATH, "/%s", filename);

    Disk[drive].isMounted = true;
    disk_read_from_sd(drive);
}

void disk_unmount(u8 drive)
{
    if (drive >= MAX_DSKS) return;

    if (Disk[drive].fp)
    {
        fflush((FILE *)Disk[drive].fp);
        fclose((FILE *)Disk[drive].fp);
        Disk[drive].fp = NULL;
    }
    Disk[drive].isMounted  = false;
    Disk[drive].isReadOnly = false;
    sector0_valid[drive]   = 0;
}

// ---------------------------------------------------------------------------
// "Read from SD" now means "open the image and keep the handle". Try read/write
// first so SAVE works; fall back to read-only for a write-protected card or a
// file that is open elsewhere, and remember which we got.
// ---------------------------------------------------------------------------
void disk_read_from_sd(u8 drive)
{
    if (drive >= MAX_DSKS) return;

    sector0_valid[drive] = 0;

    FILE *fp = fopen(Disk[drive].fullpath, "rb+");
    if (fp)
    {
        Disk[drive].isReadOnly = 0;
    }
    else
    {
        fp = fopen(Disk[drive].fullpath, "rb");
        Disk[drive].isReadOnly = 1;
    }

    Disk[drive].fp = fp;
    if (!fp)
    {
        Disk[drive].isMounted = false;
        DS_Print(3, 0, 0, "DISK MOUNT FAILED");
        return;
    }

    // Prime the sector 0 cache so the first DSR call does not pay for it.
    u8 tmp[256];
    ReadSector(drive, 0, tmp);
}

// ---------------------------------------------------------------------------
// Sector writes already went straight to the card, so this is only an fsync.
// Kept because the menu and unmount paths call it.
// ---------------------------------------------------------------------------
void disk_write_to_sd(u8 drive)
{
    if (drive >= MAX_DSKS) return;
    if (Disk[drive].fp) fflush((FILE *)Disk[drive].fp);
}

// ---------------------------------------------------------------------------
// Copy the mounted image to bak/<name>. Streamed through fileBuf rather than
// the in-memory image upstream used.
// ---------------------------------------------------------------------------
void disk_backup_to_sd(u8 drive)
{
    if (drive >= MAX_DSKS || !Disk[drive].fp) return;

    char bakdir[MAX_PATH];
    if (Disk[drive].path[0] && strcmp(Disk[drive].path, "/") != 0)
        snprintf(bakdir, MAX_PATH, "%s/bak", Disk[drive].path);
    else
        snprintf(bakdir, MAX_PATH, "/bak");
    ti99_mkdir(bakdir);

    snprintf(tmpBuf, MAX_PATH, "%s/%s", bakdir, Disk[drive].filename);
    remove(tmpBuf);

    FILE *outfile = fopen(tmpBuf, "wb");
    if (!outfile) return;

    FILE *in = (FILE *)Disk[drive].fp;
    fflush(in);
    fseek(in, 0, SEEK_SET);
    for (;;)
    {
        size_t got = fread(fileBuf, 1, sizeof(fileBuf), in);
        if (got == 0) break;
        if (fwrite(fileBuf, 1, got, outfile) != got) break;
    }
    fclose(outfile);
    sector0_valid[drive] = 0;   // the shared handle has been seeked around
}

// ------------------------------------------------------------------------
// Compute the number of used sectors by looking at the disk sector bitmap.
// ------------------------------------------------------------------------
u16 disk_get_used_sectors(u8 drive, u16 numSectors)
{
    u16 usedSectors = 0;

    u8 sec0[256];
    ReadSector(drive, 0, sec0);

    for (u16 i=0; i<numSectors/8; i++)
    {
        usedSectors += __builtin_popcount(sec0[0x38+i]);
    }

    return usedSectors;
}

// ----------------------------------------------------------------------
// Utility function to get a list of current files on the mounted disk.
// DS994a will use this listing to present a list of files to the user
// so they can pick a file and easily paste it into the keyboard buffer.
// ----------------------------------------------------------------------
DiskList_t dsk_listing[MAX_FILES_PER_DSK];   // We store the disk listing here...
u8         dsk_num_files = 0;                // And this is how many files we found (never more than MAX_FILES_PER_DSK)

void disk_get_file_listing(u8 drive)
{
    u16 sectorPtr = 0;
    u8  fdi[256];               // Sector 1: the file descriptor index

    dsk_num_files = 0;
    if (drive >= MAX_DSKS || !Disk[drive].fp) return;

    ReadSector(drive, 1, fdi);

    for (u16 i=0; i<256; i += 2)
    {
        sectorPtr = (fdi[i+0] << 8) | (fdi[i+1] << 0);
        if (sectorPtr == 0) break;

        ReadSector(drive, sectorPtr, fileBuf);
        for (u8 j=0; j<10; j++)
        {
            dsk_listing[dsk_num_files].filename[j] = fileBuf[j];
        }
        dsk_listing[dsk_num_files].filesize = (fileBuf[0xE] << 8) + fileBuf[0xF];
        dsk_listing[dsk_num_files].filename[10] = 0; // Make sure it's NULL terminated
        if (++dsk_num_files >= MAX_FILES_PER_DSK) break;
    }
}


// -------------------------------------------------------------------------
// This represents the first sectors of a 360K blank disk (sector 0 and 1).
// The rest of the disk is simply full of 0xE5 bytes when blank...
// -------------------------------------------------------------------------
static const unsigned char blank_360k[0x200] =
{
  0x42, 0x4C, 0x41, 0x4E, 0x4B, 0x5F, 0x33, 0x36,   0x30, 0x4B, 0x05, 0xA0, 0x12, 0x44, 0x53, 0x4B,
  0x20, 0x28, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,

  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

static u8 does_file_exist(char *filename)
{
    return ti99_file_exists(filename);
}

void disk_create_blank(void)
{
    int blankNum = 0;

    DS_Print(7,0,0, "CREATING DISK...");
    
    // -----------------------------------------------------------------------------------
    // Find the first available blank disk slot by examining files already on the disk.
    // We are going to create a file whose name is 'BLANK_x' where 'x' is a single letter
    // from A to Z. So after 26 blank disks, we give up (at that point, someone should 
    // have taken the SD card and used something like TI99dir to clean it up.
    // -----------------------------------------------------------------------------------
    for (blankNum = 0; blankNum < 26; blankNum++)
    {
        sprintf(tmpBuf, "BLANK_%c.DSK", 'A'+blankNum);
        if (!does_file_exist(tmpBuf))
        {
            break;
        }
    }
    
    if (blankNum < 26)
    {
        FILE *outfile = fopen(tmpBuf, "wb");
        if (outfile)
        {
            fwrite(blank_360k, 1, sizeof(blank_360k), outfile);

            // The rest of a blank disk is 0xE5 filler. Upstream writes it a byte at a
            // time through libfat's buffering; over FatFS that would be ~360k calls,
            // so fill in fileBuf-sized blocks instead.
            memset(fileBuf, 0xE5, sizeof(fileBuf));
            size_t remaining = (360 * 1024) - sizeof(blank_360k);
            while (remaining)
            {
                size_t chunk = (remaining > sizeof(fileBuf)) ? sizeof(fileBuf) : remaining;
                if (fwrite(fileBuf, 1, chunk, outfile) != chunk) break;
                remaining -= chunk;
            }
            fclose(outfile);
            DS_Print(7,0,0, "CREATING DISK...OK");
        }
        else
        {
            DS_Print(7,0,0, "CREATING DISK...FAIL");
        }
    }
    else
    {
        DS_Print(7,0,0, "CREATING DISK...FAIL");
    }
    DS_Print(7,0,0, "                    ");
}
// End of file
