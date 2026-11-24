#ifndef FAT_H
#define FAT_H
#include "usb_storage.h"
#include "namespace.h"
#include "nucleus_errors.h"
#define DIR FF_DIR
#include "vendor/fatfs/ff.h"
#undef DIR
#define FAT_MAX_OPEN_FILES 16
#define FAT_MAX_MOUNTED_FILESYSTEMS USB_VOLUME_COUNT

struct fat_fs {
    FATFS volume;
    struct usb_softc *sc;
    uint32_t generation;
    uint8_t valid, slot;
    char drive_letter;
};

struct fat_stat {
    uint32_t size, first_cluster;
    uint16_t create_time, create_date, modify_time, modify_date;
    uint8_t attributes;
};

struct fat_dir {
    FF_DIR dir;
    FILINFO info;
    struct fat_fs *filesystem;
    uint32_t generation;
    int single;
    char parent[USB_PATH_MAX], pattern[256];
};

int fat_error(FRESULT r);
int fat_scan_mbr(struct usb_softc *sc);
void fat_detach(struct usb_softc *sc);
int fat_open_flags(struct fat_fs *fs, const char *path, unsigned flags);
int fat_open(struct fat_fs *fs, const char *path);
int fat_read(int fd, void *buf, size_t count);
int fat_write(int fd, const void *buf, size_t count);
int fat_close(int fd);
int fat_seek(int fd, int32_t offset, int whence);
int fat_flush(int fd);
int fat_truncate(int fd, uint32_t size);
int fat_stat_path(struct fat_fs *fs, const char *path, struct fat_stat *st);
int fat_fstat(int fd, struct fat_stat *st);
int fat_mkdir(struct fat_fs *fs, const char *path);
int fat_unlink(struct fat_fs *fs, const char *path);
int fat_rename(struct fat_fs *fs, const char *old, const char *next);
int fat_chmod(struct fat_fs *fs, const char *path, uint8_t attr);
int fat_utime(struct fat_fs *fs, const char *path, uint16_t date, uint16_t time);
int fat_dir_open(struct fat_dir *dir, struct fat_fs *fs, const char *path, const char *pattern);
int fat_dir_next(struct fat_dir *dir);
int fat_dir_stat(struct fat_dir *dir, struct fat_fs *fs, const char *path);
void fat_dir_close(struct fat_dir *dir);
struct fat_fs *fat_get_mounted_filesystems(void);
#endif
