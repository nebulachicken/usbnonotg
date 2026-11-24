#ifndef USB_OS_ABI_H
#define USB_OS_ABI_H

#include <stddef.h>
#include <stdint.h>

/* Nucleus CX II ABI v1. Other layouts need their own adapter/review. */
enum nucleus_file_operation {
    OP_INIT,
    OP_MOUNT,
    OP_UNMOUNT,
    OP_ABORT,
    OP_OPEN,
    OP_READ,
    OP_WRITE,
    OP_SEEK,
    OP_CLOSE,
    OP_DELETE,
    OP_UTIME,
    OP_SETATTR,
    OP_GETATTR,
    OP_TRUNCATE,
    OP_FLUSH,
    OP_MKDIR,
    OP_RMDIR,
    OP_RENAME,
    OP_FIRST,
    OP_NEXT,
    OP_DONE,
    OP_FORMAT,
    OP_FORMATINFO,
    OP_FREE,
    OP_ALLOCNODE,
    OP_DEALLOCNODE,
    OP_NODESTRING,
    OP_RELEASE,
    OP_RECLAIM,
    OP_COUNT
};

struct nucleus_filesystem_ops {
    uintptr_t fn[OP_COUNT];
};

struct nucleus_mount {
    struct nucleus_filesystem_ops *ops;
    void *filesystem;
    void *device;
    uint32_t flags;
    uint32_t refs;
    uint16_t letter;
    uint16_t drvno;
    uint32_t reserved;
    uint32_t id;
    char mountpoint[2];
    char device_name[6];
};

_Static_assert(sizeof(struct nucleus_filesystem_ops) == 116, "filesystem ABI");
_Static_assert(sizeof(struct nucleus_mount) == 40, "mount ABI");
_Static_assert(offsetof(struct nucleus_mount, drvno) == 0x16, "drive ABI");

#endif
