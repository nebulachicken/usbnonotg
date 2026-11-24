/* Host regression tests: in-memory USB disks, real vendored FatFs. */
#include "fat.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char image[2][256 * 512];
static struct usb_softc devices[2];
static int fail_io;

int os_integration_mount(struct fat_fs *fs)
{
    (void)fs;
    return 0;
}

int usb_storage_scsi_sync(struct usb_softc *sc)
{
    (void)sc;
    return fail_io ? -1 : 0;
}

int usb_storage_scsi_blocks(struct usb_softc *sc, uint32_t lba, uint16_t count, void *buf,
                            size_t len, int write)
{
    unsigned which = sc == &devices[0] ? 0 : 1;
    if (fail_io || lba >= 256 || count > 256 - lba || len != (size_t)count * 512) {
        return -1;
    }
    if (write) {
        memcpy(image[which] + lba * 512, buf, len);
    } else {
        memcpy(buf, image[which] + lba * 512, len);
    }
    return 0;
}

int usb_storage_scsi_read_sectors(struct usb_softc *sc, uint32_t lba, uint16_t count, void *buf,
                                  size_t len)
{
    return len < (size_t)count * 512
               ? -1
               : usb_storage_scsi_blocks(sc, lba, count, buf, (size_t)count * 512, 0);
}

static void u16(unsigned char *p, unsigned n)
{
    p[0] = n;
    p[1] = n >> 8;
}

static void u32(unsigned char *p, unsigned n)
{
    u16(p, n);
    u16(p + 2, n >> 16);
}

static void fixture(unsigned which, int mbr)
{
    unsigned char *b, *fat, *root;
    unsigned start = mbr ? 8 : 0;
    memset(image[which], 0, sizeof(image[which]));
    memset(&devices[which], 0, sizeof(devices[which]));
    devices[which].sc_media.valid = 1;
    devices[which].sc_media.block_size = 512;
    devices[which].sc_media.last_lba = 255;
    b = image[which] + start * 512;
    b[0] = 0xeb;
    b[1] = 0x3c;
    b[2] = 0x90;
    memcpy(b + 3, "MSDOS5.0", 8);
    u16(b + 11, 512);
    b[13] = 1;
    u16(b + 14, 1);
    b[16] = 1;
    u16(b + 17, 32);
    u16(b + 19, 128);
    b[21] = 0xf8;
    u16(b + 22, 1);
    b[38] = 0x29;
    memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55;
    b[511] = 0xaa;
    fat = b + 512;
    fat[0] = 0xf8;
    fat[1] = 0xff;
    fat[2] = 0xff;
    fat[3] = 0xff;
    fat[4] = 0x0f;
    root = b + 1024;
    memcpy(root, "HELLO   TXT", 11);
    root[11] = AM_ARC;
    u16(root + 26, 2);
    u32(root + 28, 5);
    memcpy(b + 2048, "hello", 5);
    if (mbr) {
        unsigned char *m = image[which];
        m[446 + 4] = 1;
        u32(m + 446 + 8, start);
        u32(m + 446 + 12, 128);
        m[510] = 0x55;
        m[511] = 0xaa;
    }
}

static void paths(void)
{
    char out[USB_PATH_MAX];
    const char *rel;
    assert(!usb_path_normalize("a:\\documents\\usbA\\x\\..\\test.txt", "A:/", out));
    assert(!strcmp(out, "A:/documents/usbA/test.txt"));
    assert(usb_path_volume(out, &rel) == 0 && !strcmp(rel, "test.txt"));
    assert(!usb_path_normalize("../../usbB/file", "A:/documents/usbA/subdir", out));
    assert(!strcmp(out, "A:/documents/usbB/file") && usb_path_volume(out, NULL) == 1);
    assert(usb_path_volume("A:/documents/usbAbad/x", NULL) == -1);
    assert(usb_path_volume("A:/documents/usbD/x", NULL) == -1);
    assert(usb_path_normalize("B:/documents/usbA/x", "A:/", out) == -1);
    assert(!usb_path_normalize("../../..", "A:/documents", out) && !strcmp(out, "A:/"));
}

int main(void)
{
    struct fat_fs *fs = fat_get_mounted_filesystems();
    struct fat_stat st;
    struct fat_dir dir;
    char data[32];
    int fd, old, newfd, found = 0;
    paths();
    fixture(0, 0);
    assert(fat_scan_mbr(&devices[0]) == 1);
    assert(!fat_stat_path(fs, "", &st) && (st.attributes & AM_DIR));
    assert(!fat_dir_stat(&dir, fs, ""));
    assert(dir.info.fattrib & AM_DIR);
    fat_dir_close(&dir);
    fd = fat_open(fs, "HELLO.TXT");
    assert(fd > 0);
    assert(fat_write(fd, "!", 1) == NUF_ACCESS_DENIED);
    assert(fat_read(fd, data, 5) == 5 && !memcmp(data, "hello", 5));
    assert(!fat_close(fd));
    assert(fat_open(fs, "missing.txt") == NUF_NO_FILE);
    fd = fat_open_flags(fs, "new.txt", 2 | 0x100 | 0x200);
    assert(fd > 0);
    assert(fat_write(fd, "abc", 3) == 3);
    assert(fat_seek(fd, 10, 0) == 10);
    assert(!fat_fstat(fd, &st) && st.size == 3);
    assert(fat_write(fd, "Z", 1) == 1);
    assert(fat_seek(fd, 0, 0) == 0);
    memset(data, 0xff, sizeof(data));
    assert(fat_read(fd, data, 11) == 11);
    assert(!memcmp(data, "abc\0\0\0\0\0\0\0Z", 11));
    assert(!fat_close(fd));
    assert(fat_open_flags(fs, "new.txt", 1 | 0x100 | 0x400) == NUF_EXISTS);
    assert(fat_open_flags(fs, "absent.txt", 1 | 0x200) == NUF_NO_FILE);
    fd = fat_open_flags(fs, "new.txt", 1 | 8);
    assert(fd > 0);
    assert(fat_seek(fd, 0, 0) == 0 && fat_write(fd, "!", 1) == 1);
    assert(!fat_close(fd));
    assert(!fat_mkdir(fs, "Long name caf\xc3\xa9"));
    fd = fat_open_flags(fs, "Long name caf\xc3\xa9/nested.txt", 1 | 0x100);
    assert(fd > 0);
    assert(!fat_close(fd));
    assert(!fat_dir_open(&dir, fs, "", "*"));
    do {
        if (!strcmp(dir.info.fname, "Long name caf\xc3\xa9")) {
            found = 1;
        }
    } while (!fat_dir_next(&dir));
    fat_dir_close(&dir);
    assert(found);
    assert(!fat_rename(fs, "new.txt", "renamed.txt"));
    assert(!fat_stat_path(fs, "renamed.txt", &st) && st.size == 12);
    fixture(1, 1);
    assert(fat_scan_mbr(&devices[1]) == 1);
    assert(fs[0].sc == &devices[0] && fs[1].sc == &devices[1]);
    old = fat_open(fs, "HELLO.TXT");
    assert(old > 0);
    fat_detach(&devices[0]);
    assert(fat_read(old, data, 1) < 0);
    assert(fat_scan_mbr(&devices[0]) == 1);
    newfd = fat_open(fs, "HELLO.TXT");
    assert(newfd > 0 && newfd != old);
    assert(fat_read(old, data, 1) < 0);
    assert(!fat_close(old));
    assert(!fat_close(newfd));
    fat_detach(&devices[0]);
    fat_detach(&devices[1]);
    fail_io = 1;
    assert(fat_scan_mbr(&devices[0]) < 0);
    puts("namespace, FAT I/O, directories, partitions and stale-handle tests passed");
    return 0;
}
