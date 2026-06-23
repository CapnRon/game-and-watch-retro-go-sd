/*
 * G&W savestate storage backend — file-backed crash-safe ping-pong slots.
 *
 * Implements the platform_savestate_* contract (src/platform/platform.h): the
 * upstream state_dump engine writes a ~141 KiB suspend/resume snapshot through
 * two SAVESTATE_SLOTS, targeting the inactive slot and reading the newest valid
 * one, so a power loss mid-write leaves the prior slot intact.
 *
 * Unlike the Unix port's fixed "savestate.bin.<slot>" names, a retro-go build
 * has MANY user-visible save slots (and a separate sleep snapshot). We therefore
 * key the two ping-pong files on a caller-supplied *base path* set via
 * eb_savestate_set_base() before each capture/load:
 *   slot 0 -> "<base>"        (the launcher's own slot path, so its existing
 *                              file-exists slot-occupancy check still works)
 *   slot 1 -> "<base>.1"      (the second ping-pong copy)
 *
 * I/O goes through FatFs directly (f_open/f_lseek/f_write/f_sync/f_close) rather
 * than newlib stdio. The stdio path (fopen + hundreds of buffered fseek+fwrite
 * + a final fflush) failed for the large streaming snapshot: every fwrite
 * reported success but the closing fflush() returned -1 on a nearly-empty card —
 * a newlib buffering/seek interaction, not a disk error. FatFs is what stdio
 * calls underneath, is natively offset-addressed (1:1 with this API), and
 * returns precise FRESULT codes.
 */

#include <stdio.h>
#include <string.h>

#include "ff.h"
#include "platform/platform.h"
#include "main_earthbound.h"

/* Base path for the current logical savestate; the two ping-pong files derive
 * from it. Set by eb_savestate_set_base() before host_request_capture/load. */
static char ss_base[256];
static char ss_slot_path[260];

void eb_savestate_set_base(const char *path)
{
    if (!path) {
        ss_base[0] = '\0';
        return;
    }
    strncpy(ss_base, path, sizeof(ss_base) - 1);
    ss_base[sizeof(ss_base) - 1] = '\0';
}

static const char *slot_path(int slot)
{
    if (ss_base[0] == '\0')
        return NULL;
    if (slot == 0)
        return ss_base;
    snprintf(ss_slot_path, sizeof(ss_slot_path), "%s.%d", ss_base, slot);
    return ss_slot_path;
}

/* One open writer at a time. The engine writes a single slot to completion
 * (begin -> writes -> commit) before touching another, so one FIL suffices;
 * ss_wslot tracks which slot owns it (-1 = none open). FF_FS_TINY makes FIL
 * small (no per-file sector buffer), so this costs almost no RAM. */
static FIL ss_wfil;
static int ss_wslot = -1;

bool platform_savestate_begin(int slot)
{
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return false;
    const char *path = slot_path(slot);
    if (!path)
        return false;
    if (ss_wslot >= 0) {            /* a prior capture never committed — drop it */
        f_close(&ss_wfil);
        ss_wslot = -1;
    }
    FRESULT r = f_open(&ss_wfil, path, FA_WRITE | FA_CREATE_ALWAYS); /* truncate */
    if (r != FR_OK)
        return false;
    ss_wslot = slot;
    return true;
}

bool platform_savestate_write(int slot, size_t offset, const void *src, size_t size)
{
    if (slot != ss_wslot)
        return false;
    if (f_lseek(&ss_wfil, (FSIZE_t)offset) != FR_OK)
        return false;
    UINT bw = 0;
    if (f_write(&ss_wfil, src, (UINT)size, &bw) != FR_OK || bw != size)
        return false;
    return true;
}

bool platform_savestate_commit(int slot)
{
    if (slot != ss_wslot)
        return false;
    FRESULT rs = f_sync(&ss_wfil);
    FRESULT rc = f_close(&ss_wfil);
    ss_wslot = -1;
    return (rs == FR_OK && rc == FR_OK);
}

size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size)
{
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return 0;
    const char *path = slot_path(slot);
    if (!path)
        return 0;
    FIL f;
    if (f_open(&f, path, FA_READ) != FR_OK)
        return 0;
    UINT br = 0;
    if (f_lseek(&f, (FSIZE_t)offset) == FR_OK)
        f_read(&f, dst, (UINT)size, &br);
    f_close(&f);
    return br;
}
