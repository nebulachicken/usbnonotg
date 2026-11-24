#include "fat.h"
#include "vendor/fatfs/diskio.h"
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>

DWORD get_fattime(void)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (!t || t->tm_year < 80 || t->tm_year > 207) {
        return ((2025 - 1980) << 25) | (1 << 21) | (1 << 16);
    }
    return ((DWORD)(t->tm_year - 80) << 25) | ((DWORD)(t->tm_mon + 1) << 21) |
           ((DWORD)t->tm_mday << 16) | ((DWORD)t->tm_hour << 11) | ((DWORD)t->tm_min << 5) |
           (t->tm_sec / 2);
}

PARTITION VolToPart[USB_VOLUME_COUNT];
static struct usb_softc *disks[USB_VOLUME_COUNT];
static struct fat_fs volumes[USB_VOLUME_COUNT];
/* Mount and detach run under the same global lock; avoid a 4K USB-task stack buffer. */
static uint8_t mount_sector[4096];

static struct {
    FIL file;
    struct fat_fs *filesystem;
    uint32_t generation;
    uint32_t pos;
    unsigned used, append;
} files[FAT_MAX_OPEN_FILES];

int fat_error(FRESULT result)
{
    switch (result) {
    case FR_OK:
        return 0;
    case FR_NO_FILE:
    case FR_NO_PATH:
        return NUF_NO_FILE;
    case FR_DENIED:
    case FR_WRITE_PROTECTED:
        return NUF_ACCESS_DENIED;
    case FR_EXIST:
        return NUF_EXISTS;
    case FR_TOO_MANY_OPEN_FILES:
        return NUF_TOO_MANY_FILES;
    case FR_INVALID_OBJECT:
        return NUF_BAD_DESCRIPTOR;
    case FR_NOT_READY:
    case FR_INVALID_DRIVE:
    case FR_NOT_ENABLED:
        return NUF_IO_ERROR;
    case FR_INVALID_NAME:
    case FR_INVALID_PARAMETER:
        return NUF_INVALID_PARAMETER;
    default:
        return NUF_IO_ERROR;
    }
}

DSTATUS disk_status(BYTE p)
{
    if (p >= USB_VOLUME_COUNT || !disks[p] || !disks[p]->sc_media.valid) {
        return STA_NOINIT;
    }
    return disks[p]->sc_media.write_protected ? STA_PROTECT : 0;
}

DSTATUS disk_initialize(BYTE p)
{
    return disk_status(p);
}

static DRESULT disk_transfer(BYTE p, void *buf, LBA_t sector, UINT count, int write)
{
    struct usb_softc *sc;
    uint8_t *bytes = buf;
    DSTATUS status = disk_status(p);
    if (status & STA_NOINIT) {
        return RES_NOTRDY;
    }
    if (write && (status & STA_PROTECT)) {
        return RES_WRPRT;
    }
    sc = disks[p];
    if (!count || !buf || sector > sc->sc_media.last_lba ||
        count - 1 > sc->sc_media.last_lba - sector) {
        return RES_PARERR;
    }
    while (count) {
        UINT n = 16384 / sc->sc_media.block_size;
        if (n > count) {
            n = count;
        }
        if (usb_storage_scsi_blocks(sc, sector, n, bytes, n * sc->sc_media.block_size, write)) {
            return RES_ERROR;
        }
        sector += n;
        count -= n;
        bytes += n * sc->sc_media.block_size;
    }
    return RES_OK;
}

DRESULT disk_read(BYTE p, BYTE *b, LBA_t s, UINT n)
{
    return disk_transfer(p, b, s, n, 0);
}

DRESULT disk_write(BYTE p, const BYTE *b, LBA_t s, UINT n)
{
    return disk_transfer(p, (void *)b, s, n, 1);
}

DRESULT disk_ioctl(BYTE p, BYTE cmd, void *buf)
{
    if (disk_status(p) & STA_NOINIT) {
        return RES_NOTRDY;
    }
    if (cmd == CTRL_SYNC) {
        return usb_storage_scsi_sync(disks[p]) ? RES_ERROR : RES_OK;
    }
    if (!buf) {
        return RES_PARERR;
    }
    switch (cmd) {
    case GET_SECTOR_SIZE:
        *(WORD *)buf = disks[p]->sc_media.block_size;
        break;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = disks[p]->sc_media.last_lba + 1;
        break;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        break;
    default:
        return RES_PARERR;
    }
    return RES_OK;
}

static int path_for(struct fat_fs *filesystem, const char *path, char out[USB_PATH_MAX])
{
    if (!filesystem || !filesystem->valid || !path) {
        return NUF_NO_FILE;
    }
    if (snprintf(out, USB_PATH_MAX, "%u:/%s", filesystem->slot, path) >= USB_PATH_MAX) {
        return NUF_BAD_PARAMETER;
    }
    return 0;
}

static int file_index(int fd)
{
    int index = fd - 1;
    return index >= 0 && index < FAT_MAX_OPEN_FILES && files[index].used ? index : -1;
}

static int live_file(int fd)
{
    int index = file_index(fd);
    return index >= 0 && files[index].filesystem->valid &&
                   files[index].generation == files[index].filesystem->generation
               ? index
               : -1;
}

int fat_scan_mbr(struct usb_softc *sc)
{
    uint8_t *sector = mount_sector;
    unsigned partitions[4], n = 0, i, slot;
    int mounted = 0;
    if (!sc || !sc->sc_media.valid || sc->sc_media.block_size > sizeof(mount_sector)) {
        return -1;
    }
    if (usb_storage_scsi_read_sectors(sc, 0, 1, sector, sizeof(mount_sector))) {
        return -1;
    }
    if (sector[510] == 0x55 && sector[511] == 0xaa && (sector[0] == 0xeb || sector[0] == 0xe9) &&
        sector[13] && sector[16] &&
        ((unsigned)sector[11] | ((unsigned)sector[12] << 8)) == sc->sc_media.block_size) {
        partitions[n++] = 0;
    } else if (sector[510] == 0x55 && sector[511] == 0xaa) {
        for (i = 0; i < 4; ++i) {
            unsigned type = sector[446 + 16 * i + 4];
            if (type == 1 || type == 4 || type == 6 || type == 11 || type == 12 || type == 14) {
                partitions[n++] = i + 1;
            }
        }
    }
    for (i = 0; i < n; ++i) {
        char drive[4];
        FRESULT r;
        for (slot = 0; slot < USB_VOLUME_COUNT && volumes[slot].valid; ++slot) {
        }
        if (slot == USB_VOLUME_COUNT) {
            break;
        }
        volumes[slot].slot = slot;
        volumes[slot].sc = sc;
        disks[slot] = sc;
        VolToPart[slot].pd = slot;
        VolToPart[slot].pt = partitions[i];
        snprintf(drive, sizeof(drive), "%u:", slot);
        r = f_mount(&volumes[slot].volume, drive, 1);
        if (r != FR_OK) {
            f_mount(NULL, drive, 0);
            disks[slot] = NULL;
            continue;
        }
        ++volumes[slot].generation;
        volumes[slot].valid = 1;
        if (os_integration_mount(&volumes[slot])) {
            volumes[slot].valid = 0;
            f_mount(NULL, drive, 0);
            disks[slot] = NULL;
            continue;
        }
        ++mounted;
    }
    return mounted;
}

void fat_detach(struct usb_softc *sc)
{
    unsigned i;
    for (i = 0; i < USB_VOLUME_COUNT; ++i) {
        if (volumes[i].sc == sc) {
            char drive[4];
            volumes[i].valid = 0;
            ++volumes[i].generation;
            disks[i] = NULL;
            volumes[i].sc = NULL;
            snprintf(drive, sizeof(drive), "%u:", i);
            f_mount(NULL, drive, 0);
            /* Reserve old file IDs until close so hotplug cannot alias an old handle. */
        }
    }
}

int fat_open_flags(struct fat_fs *filesystem, const char *path, unsigned flags)
{
    char full_path[USB_PATH_MAX];
    unsigned index;
    BYTE mode;
    int result = path_for(filesystem, path, full_path);
    if (result) {
        return result;
    }
    if ((flags & NUF_OPEN_ACCESS_MASK) == 3 ||
        ((flags & (NUF_OPEN_TRUNCATE | NUF_OPEN_APPEND)) && !(flags & NUF_OPEN_ACCESS_MASK)) ||
        ((flags & NUF_OPEN_EXCLUSIVE) && !(flags & NUF_OPEN_CREATE))) {
        return NUF_INVALID_PARAMETER;
    }
    switch (flags & NUF_OPEN_ACCESS_MASK) {
    case NUF_OPEN_READ:
        mode = FA_READ;
        break;
    case NUF_OPEN_WRITE:
        mode = FA_WRITE;
        break;
    case NUF_OPEN_READWRITE:
        mode = FA_READ | FA_WRITE;
        break;
    default:
        return NUF_INVALID_PARAMETER;
    }
    if (flags & NUF_OPEN_EXCLUSIVE) {
        mode |= FA_CREATE_NEW;
    } else if (flags & NUF_OPEN_TRUNCATE) {
        mode |= FA_CREATE_ALWAYS;
    } else if (flags & NUF_OPEN_CREATE) {
        mode |= FA_OPEN_ALWAYS;
    }
    if ((flags & NUF_OPEN_TRUNCATE) && !(flags & NUF_OPEN_CREATE)) {
        FILINFO info;
        result = fat_error(f_stat(full_path, &info));
        if (result) {
            return result;
        }
    }
    for (index = 0; index < FAT_MAX_OPEN_FILES && files[index].used; ++index) {
    }
    if (index == FAT_MAX_OPEN_FILES) {
        return NUF_TOO_MANY_FILES;
    }
    result = fat_error(f_open(&files[index].file, full_path, mode));
    if (result) {
        return result;
    }
    if (f_size(&files[index].file) > INT_MAX) {
        f_close(&files[index].file);
        return NUF_INVALID_PARAMETER;
    }
    files[index].filesystem = filesystem;
    files[index].generation = filesystem->generation;
    files[index].used = 1;
    files[index].append = !!(flags & NUF_OPEN_APPEND);
    files[index].pos = files[index].append ? f_size(&files[index].file) : 0;
    return index + 1;
}

int fat_open(struct fat_fs *fs, const char *path)
{
    return fat_open_flags(fs, path, 0);
}

int fat_read(int fd, void *buffer, size_t count)
{
    int index = live_file(fd);
    UINT done;
    FRESULT result;
    if (index < 0) {
        return NUF_BAD_DESCRIPTOR;
    }
    if ((!buffer && count) || count > INT_MAX) {
        return NUF_BAD_PARAMETER;
    }
    if (!(files[index].file.flag & FA_READ)) {
        return NUF_ACCESS_DENIED;
    }
    if (files[index].pos >= f_size(&files[index].file)) {
        return 0;
    }
    result = f_lseek(&files[index].file, files[index].pos);
    if (result != FR_OK) {
        return fat_error(result);
    }
    result = f_read(&files[index].file, buffer, count, &done);
    files[index].pos += done;
    return result == FR_OK ? (int)done : fat_error(result);
}

int fat_write(int fd, const void *buffer, size_t count)
{
    int index = live_file(fd);
    UINT done;
    FRESULT result;
    if (index < 0) {
        return NUF_BAD_DESCRIPTOR;
    }
    if ((!buffer && count) || count > INT_MAX) {
        return NUF_BAD_PARAMETER;
    }
    if (!(files[index].file.flag & FA_WRITE)) {
        return NUF_ACCESS_DENIED;
    }
    if (files[index].append) {
        files[index].pos = f_size(&files[index].file);
    }
    if (files[index].pos > INT_MAX || count > (uint32_t)INT_MAX - files[index].pos) {
        return NUF_INVALID_PARAMETER;
    }
    if (!count) {
        return 0;
    }
    /* Vendored FatFs zero-fills any gap when an extending seek is needed. */
    result = f_lseek(&files[index].file, files[index].pos);
    if (result != FR_OK) {
        return fat_error(result);
    }
    if (f_tell(&files[index].file) != files[index].pos) {
        return NUF_NO_SPACE;
    }
    result = f_write(&files[index].file, buffer, count, &done);
    files[index].pos += done;
    if (result == FR_OK && !done && count) {
        return NUF_NO_SPACE;
    }
    return result == FR_OK ? (int)done : fat_error(result);
}

int fat_close(int fd)
{
    int index = file_index(fd), result = 0;
    if (index < 0) {
        return NUF_BAD_DESCRIPTOR;
    }
    if (live_file(fd) >= 0) {
        result = fat_error(f_close(&files[index].file));
    }
    /* NU_Close frees its descriptor even if the callback reports a flush error. */
    if (result) {
        struct usb_softc *sc = files[index].filesystem->sc;
        if (sc) {
            sc->sc_media.valid = 0;
            fat_detach(sc);
        }
    }
    memset(&files[index], 0, sizeof(files[index]));
    return result;
}

int fat_seek(int fd, int32_t offset, int whence)
{
    int index = live_file(fd);
    int64_t position;
    if (index < 0) {
        return NUF_BAD_DESCRIPTOR;
    }
    if (whence < 0 || whence > 2) {
        return NUF_BAD_PARAMETER;
    }
    position = (whence == 0   ? 0
                : whence == 1 ? (int64_t)files[index].pos
                              : (int64_t)f_size(&files[index].file)) +
               offset;
    if (position < 0 || position > INT_MAX) {
        return NUF_BAD_PARAMETER;
    }
    /* Match Reliance: seek updates position without extending the file. */
    files[index].pos = (uint32_t)position;
    return (int)position;
}

int fat_flush(int fd)
{
    int index = live_file(fd);
    return index < 0 ? NUF_BAD_DESCRIPTOR : fat_error(f_sync(&files[index].file));
}

int fat_truncate(int fd, uint32_t size)
{
    int index = live_file(fd);
    FRESULT result;
    FSIZE_t old_position;
    if (index < 0) {
        return NUF_BAD_DESCRIPTOR;
    }
    if (!(files[index].file.flag & FA_WRITE)) {
        return NUF_ACCESS_DENIED;
    }
    if (size > f_size(&files[index].file)) {
        return NUF_BAD_PARAMETER;
    }
    old_position = files[index].pos;
    result = f_lseek(&files[index].file, size);
    if (result == FR_OK) {
        result = f_truncate(&files[index].file);
    }
    if (result == FR_OK) {
        result = f_lseek(&files[index].file, old_position < size ? old_position : size);
    }
    if (result == FR_OK) {
        files[index].pos = old_position < size ? old_position : size;
    }
    return fat_error(result);
}

int fat_stat_path(struct fat_fs *filesystem, const char *path, struct fat_stat *status)
{
    char full_path[USB_PATH_MAX];
    FILINFO info;
    int result = path_for(filesystem, path, full_path);
    if (result || !status) {
        return result ? result : NUF_BAD_PARAMETER;
    }
    memset(status, 0, sizeof(*status));
    if (!*path) {
        status->attributes = AM_DIR;
        return 0;
    }
    result = fat_error(f_stat(full_path, &info));
    if (!result) {
        status->size = info.fsize;
        status->attributes = info.fattrib;
        status->modify_date = info.fdate;
        status->modify_time = info.ftime;
        status->create_date = info.crdate;
        status->create_time = info.crtime;
    }
    return result;
}

int fat_fstat(int fd, struct fat_stat *status)
{
    int index = live_file(fd);
    if (index < 0 || !status) {
        return NUF_BAD_DESCRIPTOR;
    }
    memset(status, 0, sizeof(*status));
    status->size = f_size(&files[index].file);
    return 0;
}

int fat_mkdir(struct fat_fs *filesystem, const char *path)
{
    char full_path[USB_PATH_MAX];
    int result = path_for(filesystem, path, full_path);

    if (result) {
        return result;
    }
    return fat_error(f_mkdir(full_path));
}

int fat_unlink(struct fat_fs *filesystem, const char *path)
{
    char full_path[USB_PATH_MAX];
    int result = path_for(filesystem, path, full_path);

    if (result) {
        return result;
    }
    return fat_error(f_unlink(full_path));
}

int fat_rename(struct fat_fs *filesystem, const char *old_path, const char *new_path)
{
    char old_full_path[USB_PATH_MAX], new_full_path[USB_PATH_MAX];
    int result = path_for(filesystem, old_path, old_full_path);
    if (!result) {
        result = path_for(filesystem, new_path, new_full_path);
    }
    return result ? result : fat_error(f_rename(old_full_path, new_full_path));
}

int fat_chmod(struct fat_fs *filesystem, const char *path, uint8_t attributes)
{
    char full_path[USB_PATH_MAX];
    int result = path_for(filesystem, path, full_path);
    return result ? result
                  : fat_error(f_chmod(full_path, attributes, AM_RDO | AM_HID | AM_SYS | AM_ARC));
}

int fat_utime(struct fat_fs *filesystem, const char *path, uint16_t date, uint16_t time)
{
    char full_path[USB_PATH_MAX];
    FILINFO info;
    int result = path_for(filesystem, path, full_path);
    memset(&info, 0, sizeof(info));
    info.fdate = date;
    info.ftime = time;
    return result ? result : fat_error(f_utime(full_path, &info));
}

int fat_dir_open(struct fat_dir *directory, struct fat_fs *filesystem, const char *path,
                 const char *pattern)
{
    char full_path[USB_PATH_MAX];
    FRESULT result;
    int error = path_for(filesystem, path, full_path);
    if (error) {
        return error;
    }
    memset(directory, 0, sizeof(*directory));
    directory->filesystem = filesystem;
    directory->generation = filesystem->generation;
    if (strlen(path) >= sizeof(directory->parent) ||
        strlen(pattern) >= sizeof(directory->pattern)) {
        return NUF_INVALID_PARAMETER;
    }
    strcpy(directory->parent, path);
    strcpy(directory->pattern, pattern);
    result = f_findfirst(&directory->dir, &directory->info, full_path, directory->pattern);
    if (result != FR_OK) {
        f_closedir(&directory->dir);
    }
    if (result == FR_OK && !directory->info.fname[0]) {
        f_closedir(&directory->dir);
        return NUF_NO_FILE;
    }
    return fat_error(result);
}

int fat_dir_next(struct fat_dir *directory)
{
    FRESULT result;
    if (!directory->filesystem->valid ||
        directory->generation != directory->filesystem->generation) {
        return NUF_IO_ERROR;
    }
    if (directory->single) {
        return NUF_NO_FILE;
    }
    result = f_findnext(&directory->dir, &directory->info);
    return result == FR_OK && !directory->info.fname[0] ? NUF_NO_FILE : fat_error(result);
}

int fat_dir_stat(struct fat_dir *directory, struct fat_fs *filesystem, const char *path)
{
    char full_path[USB_PATH_MAX];
    const char *name;
    char *last_separator;
    int result = path_for(filesystem, path, full_path);
    if (result) {
        return result;
    }
    memset(directory, 0, sizeof(*directory));
    directory->filesystem = filesystem;
    directory->generation = filesystem->generation;
    directory->single = 1;
    if (!*path) {
        directory->info.fattrib = AM_DIR;
        directory->info.fdate = ((2025 - 1980) << 9) | (1 << 5) | 1;
        directory->info.crdate = directory->info.fdate;
        snprintf(directory->info.fname, sizeof(directory->info.fname), "usb%c",
                 'A' + filesystem->slot);
        return 0;
    }
    result = fat_error(f_stat(full_path, &directory->info));
    strcpy(directory->parent, path);
    last_separator = strrchr(directory->parent, '/');
    if (last_separator) {
        *last_separator = 0;
    } else {
        directory->parent[0] = 0;
    }
    name = strrchr(path, '/');
    name = name ? name + 1 : path;
    if (!directory->info.fname[0]) {
        strncpy(directory->info.fname, name, sizeof(directory->info.fname) - 1);
    }
    return result;
}

void fat_dir_close(struct fat_dir *directory)
{
    if (!directory->single && directory->filesystem->valid &&
        directory->generation == directory->filesystem->generation) {
        f_closedir(&directory->dir);
    }
}

struct fat_fs *fat_get_mounted_filesystems(void)
{
    return volumes;
}
