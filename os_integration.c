/* Nucleus FILE overlay using a reviewed OS profile.
 * See docs/os-6.2.0.333.md for binary evidence and ABI details.
 */
#include <os.h>
#include "fat.h"
#include "console.h"
#include "usb_diag.h"
#include <stddef.h>
#include "os_abi.h"
#include "os_runtime.h"

extern unsigned int nl_osid(void);
/* The confirmed NU_SEMAPHORE layout is 0x28 bytes, 4-byte aligned. */
static uint32_t semaphore[10];
static NU_TASK *lock_owner;
static unsigned lock_depth;
static int initialized;
static struct nucleus_mount overlays[USB_VOLUME_COUNT];
static struct nucleus_filesystem_ops usb_ops;
static struct nucleus_filesystem_ops *a_ops;
static int (*a_allocate)(uint16_t, const char *, void **);
static int (*a_deallocate)(uint16_t, void *);
static int (*a_node_string)(uint16_t, const void *, char *);
static int (*a_rename)(const char *, const char *);
static uint32_t trampoline[4] __attribute__((aligned(4)));
static struct nucleus_mount *(*original_findmount)(const char *);

void usb_storage_lock(void)
{
    NU_TASK *task = TCC_Current_Task_Pointer();
    if (task == lock_owner && lock_depth) {
        ++lock_depth;
        return;
    }
    if (usb_os_semaphore_obtain(semaphore, 0xffffffffu)) {
        abort();
    }
    lock_owner = task;
    lock_depth = 1;
}

void usb_storage_unlock(void)
{
    if (lock_depth == 1) {
        usb_diag_flush(); /* still locked, and no FatFs call is in progress */
    }
    if (--lock_depth == 0) {
        lock_owner = NULL;
        usb_os_semaphore_release(semaphore);
    }
}

/* No shared scratch buffer: hooks can run in multiple OS tasks. */
static int normalize(const char *path, char out[USB_PATH_MAX])
{
    char cwd[USB_PATH_MAX];
    const char *relative_path = path;
    int result;
    if (!relative_path || !*relative_path) {
        return -1;
    }
    /* OS libc and Reliance use 256-byte pathname buffers. */
    if (strlen(relative_path) >= 256) {
        return -1;
    }
    if (usb_os_parse_drive(path, 1) != 0) {
        return -1;
    }
    if (relative_path[1] == ':') {
        relative_path += 2;
    }
    if (*relative_path == '/' || *relative_path == '\\') {
        result = usb_path_normalize(path, "A:/", out);
    } else {
        if (NU_Current_Dir("A:", cwd)) {
            return -1;
        }
        result = usb_path_normalize(path, cwd, out);
    }
    return result || strlen(out) >= 256 ? -1 : 0;
}

static struct fat_fs *resolve(const char *path, char normalized[USB_PATH_MAX],
                              const char **relative)
{
    int volume_index;
    if (normalize(path, normalized)) {
        return NULL;
    }
    volume_index = usb_path_volume(normalized, relative);
    return volume_index < 0 ? NULL : &fat_get_mounted_filesystems()[volume_index];
}

static struct nucleus_mount *overlay_findmount(const char *path)
{
    char normalized[USB_PATH_MAX];
    int slot = -1;
    usb_storage_lock();
    if (!normalize(path, normalized)) {
        slot = usb_path_volume(normalized, NULL);
    }
    usb_storage_unlock();
    /* Also route disconnected volumes: never fall through into hidden NAND files. */
    return slot >= 0 ? &overlays[slot] : original_findmount(path);
}

/* A retains its per-file-user working directory. Reliance stores it as a path
 * string; our owned nodes use the same representation and are freed by us.
 * This permits chdir into USB and relative paths back out into Reliance.
 */
struct cwd_node {
    struct cwd_node *next;
    char path[1];
};
static struct cwd_node *nodes;

static int allocate_node(uint16_t drive, const char *path, void **out)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    struct fat_stat status;
    struct cwd_node *node;
    size_t length, index;
    int result;
    if (!out) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    if (!filesystem) {
        result = a_allocate(drive, path, out);
        goto done;
    }
    result = fat_stat_path(filesystem, relative, &status);
    if (result) {
        goto done;
    }
    if (!(status.attributes & AM_DIR)) {
        result = NUF_NO_FILE;
        goto done;
    }
    length = strlen(normalized + 2);
    if (length + 1 >= 256) {
        result = NUF_BAD_PARAMETER;
        goto done;
    }
    node = malloc(sizeof(*node) + length + 1);
    if (!node) {
        result = NUF_IO_ERROR;
        goto done;
    }
    strcpy(node->path, normalized + 2);
    for (index = 0; index < length; ++index) {
        if (node->path[index] == '/') {
            node->path[index] = '\\';
        }
    }
    if (length && node->path[length - 1] != '\\') {
        node->path[length++] = '\\';
    }
    node->path[length] = 0;
    node->next = nodes;
    nodes = node;
    *out = node->path;
    result = 0;
done:
    usb_storage_unlock();
    return result;
}

static int deallocate_node(uint16_t drive, void *pointer)
{
    struct cwd_node **link;
    int result = 0;
    usb_storage_lock();
    for (link = &nodes; *link; link = &(*link)->next) {
        if ((*link)->path == pointer) {
            struct cwd_node *node = *link;
            *link = node->next;
            free(node);
            goto done;
        }
    }
    result = a_deallocate(drive, pointer);
done:
    usb_storage_unlock();
    return result;
}

static int node_string(uint16_t drive, const void *pointer, char *out)
{
    struct cwd_node *node;
    int result;
    if (!out) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    for (node = nodes; node; node = node->next) {
        if (node->path == pointer) {
            strcpy(out, node->path);
            result = 0;
            goto done;
        }
    }
    result = a_node_string(drive, pointer, out);
done:
    usb_storage_unlock();
    return result;
}

static int fs_open(uint16_t drive, const char *path, uint16_t flags, uint16_t mode)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    int result;
    (void)drive;
    (void)mode;
    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    result = filesystem ? fat_open_flags(filesystem, relative, flags) : NUF_NO_FILE;
    usb_storage_unlock();
    return result;
}

static int fs_read(int fd, void *buffer, unsigned count)
{
    int result;

    usb_storage_lock();
    result = fat_read(fd, buffer, count);
    usb_storage_unlock();
    return result;
}

static int fs_write(int fd, const void *buffer, unsigned count)
{
    int result;

    usb_storage_lock();
    result = fat_write(fd, buffer, count);
    usb_storage_unlock();
    return result;
}

static int fs_seek(int fd, int32_t offset, int whence)
{
    int result;

    usb_storage_lock();
    result = fat_seek(fd, offset, whence);
    usb_storage_unlock();
    return result;
}

static int fs_close(int fd)
{
    int result;

    usb_storage_lock();
    result = fat_close(fd);
    usb_storage_unlock();
    return result;
}

static int fs_flush(int fd)
{
    int result;

    usb_storage_lock();
    result = fat_flush(fd);
    usb_storage_unlock();
    return result;
}

static int fs_truncate(int fd, uint32_t size)
{
    int result;

    usb_storage_lock();
    result = fat_truncate(fd, size);
    usb_storage_unlock();
    return result;
}

static int fs_mkdir(const char *path)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    int result;

    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    result = filesystem ? fat_mkdir(filesystem, relative) : NUF_NO_FILE;
    usb_storage_unlock();
    return result;
}

static int fs_unlink(const char *path)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    int result;

    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    result = filesystem ? fat_unlink(filesystem, relative) : NUF_NO_FILE;
    usb_storage_unlock();
    return result;
}

static int fs_getattr(uint8_t *attributes, const char *path)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    struct fat_stat status;
    int result;
    if (!attributes) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    result = filesystem ? fat_stat_path(filesystem, relative, &status) : NUF_NO_FILE;
    if (!result) {
        *attributes = status.attributes;
    }
    usb_storage_unlock();
    return result;
}

static int fs_setattr(const char *path, uint8_t attributes)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    struct fat_fs *filesystem;
    int result;
    usb_storage_lock();
    filesystem = resolve(path, normalized, &relative);
    result = filesystem ? fat_chmod(filesystem, relative, attributes) : NUF_NO_FILE;
    usb_storage_unlock();
    return result;
}

static int fs_rename(const char *old_path, const char *new_path)
{
    char old_normalized[USB_PATH_MAX], new_normalized[USB_PATH_MAX];
    const char *old_relative, *new_relative;
    struct fat_fs *source_fs, *destination_fs;
    int result;
    usb_storage_lock();
    source_fs = resolve(old_path, old_normalized, &old_relative);
    destination_fs = resolve(new_path, new_normalized, &new_relative);
    result = source_fs && source_fs == destination_fs
                 ? fat_rename(source_fs, old_relative, new_relative)
                 : NUF_INVALID_PARAMETER;
    usb_storage_unlock();
    return result;
}

static int a_rename_overlay(const char *old_path, const char *new_path)
{
    char old_normalized[USB_PATH_MAX], new_normalized[USB_PATH_MAX];
    const char *old_relative, *new_relative;
    struct fat_fs *source_fs, *destination_fs;
    int result;
    usb_storage_lock();
    source_fs = resolve(old_path, old_normalized, &old_relative);
    destination_fs = resolve(new_path, new_normalized, &new_relative);
    if (source_fs || destination_fs) {
        result = source_fs && source_fs == destination_fs
                     ? fat_rename(source_fs, old_relative, new_relative)
                     : NUF_INVALID_PARAMETER;
    } else {
        result = a_rename(old_path, new_path);
    }
    usb_storage_unlock();
    return result;
}

/* The SDK DSTAT declaration predates this OS; use verified offsets, never its fields. */
#define DS_CONTEXT 0x128

static void ds_pointer(void *dstat, void *pointer)
{
    memcpy((char *)dstat + DS_CONTEXT, &pointer, 4);
}

static struct fat_dir *ds_dir(void *dstat)
{
    struct fat_dir *pointer;
    memcpy(&pointer, (char *)dstat + DS_CONTEXT, 4);
    return pointer;
}

static void ds_word(void *dstat, unsigned offset, uint16_t value)
{
    memcpy((char *)dstat + offset, &value, 2);
}

static void ds_fill(void *dstat, struct fat_dir *directory)
{
    FILINFO *info = &directory->info;
    char *bytes = dstat;
    /* Leave Nucleus-owned fobj (0x124), mount ID (0x12c), drvno (0x130) intact. */
    memset(bytes, 0, 0x124);
    {
        const char *name = info->altname[0] ? info->altname : info->fname;
        const char *dot = strrchr(name, '.');
        size_t length = dot ? (size_t)(dot - name) : strlen(name);
        memset(bytes, ' ', 8);
        bytes[8] = 0;
        memset(bytes + 9, ' ', 3);
        bytes[12] = 0;
        if (length > 8) {
            length = 8;
        }
        memcpy(bytes, name, length);
        if (dot) {
            size_t extension_length = strlen(dot + 1);
            if (extension_length > 3) {
                extension_length = 3;
            }
            memcpy(bytes + 9, dot + 1, extension_length);
        }
    }
    strncpy(bytes + 0xd, info->fname, 255);
    bytes[0xd + 255] = 0;
    bytes[0x10d] = info->fattrib;
    ds_word(dstat, 0x110, info->crtime);
    ds_word(dstat, 0x112, info->crdate);
    ds_word(dstat, 0x114, info->fdate);
    ds_word(dstat, 0x118, info->ftime);
    ds_word(dstat, 0x11a, info->fdate);
    memcpy(bytes + 0x120, &info->fsize, 4);
}

static int fs_first(void *dstat, const char *path)
{
    char normalized[USB_PATH_MAX];
    const char *relative;
    char *last_separator;
    struct fat_fs *filesystem;
    struct fat_dir *directory;
    int result;
    if (!dstat) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    ds_pointer(dstat, NULL);
    filesystem = resolve(path, normalized, &relative);
    if (!filesystem) {
        result = NUF_NO_FILE;
        goto done;
    }
    directory = malloc(sizeof(*directory));
    if (!directory) {
        result = NUF_IO_ERROR;
        goto done;
    }
    if (!strchr(relative, '*') && !strchr(relative, '?')) {
        result = fat_dir_stat(directory, filesystem, relative);
    } else {
        char pattern[256];
        const char *name;
        last_separator = strrchr((char *)relative, '/');
        name = last_separator ? last_separator + 1 : relative;
        strcpy(pattern, *name ? name : "*");
        if (!strcmp(pattern, "*.*")) {
            strcpy(pattern, "*");
        }
        if (last_separator) {
            *last_separator = 0;
        } else {
            *((char *)relative) = 0;
        }
        result = fat_dir_open(directory, filesystem, relative, pattern);
    }
    if (result) {
        free(directory);
        goto done;
    }
    ds_pointer(dstat, directory);
    ds_fill(dstat, directory);
done:
    usb_storage_unlock();
    return result;
}

static int fs_next(void *dstat)
{
    struct fat_dir *directory;
    int result;
    if (!dstat) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    directory = ds_dir(dstat);
    result = directory ? fat_dir_next(directory) : NUF_NO_FILE;
    if (!result) {
        ds_fill(dstat, directory);
    }
    usb_storage_unlock();
    return result;
}

static int fs_done(void *dstat)
{
    struct fat_dir *directory;
    if (!dstat) {
        return NUF_BAD_PARAMETER;
    }
    usb_storage_lock();
    directory = ds_dir(dstat);
    if (directory) {
        fat_dir_close(directory);
        free(directory);
        ds_pointer(dstat, NULL);
    }
    usb_storage_unlock();
    return 0;
}

static int fs_utime(void *dstat, uint16_t access_date, uint16_t access_time, uint16_t modify_date,
                    uint16_t modify_time, uint16_t create_date, uint16_t create_time)
{
    struct fat_dir *directory;
    char path[USB_PATH_MAX];
    int result;
    (void)access_date;
    (void)access_time;
    (void)create_date;
    (void)create_time;
    if (!dstat) {
        return NUF_INVALID_PARAMETER;
    }
    usb_storage_lock();
    directory = ds_dir(dstat);
    if (!directory || !directory->filesystem->valid ||
        directory->generation != directory->filesystem->generation) {
        result = NUF_NO_FILE;
        goto done;
    }
    if (snprintf(path, sizeof(path), "%s%s%s", directory->parent, *directory->parent ? "/" : "",
                 directory->info.fname) >= (int)sizeof(path)) {
        result = NUF_NAME_TOO_LONG;
        goto done;
    }
    result = fat_utime(directory->filesystem, path, modify_date, modify_time);
    if (!result) {
        directory->info.fdate = modify_date;
        directory->info.ftime = modify_time;
        ds_fill(dstat, directory);
    }
done:
    usb_storage_unlock();
    return result;
}

static int unsupported(void)
{
    return NUF_BAD_PARAMETER;
}

static int success(void)
{
    return 0;
}

int os_integration_mount(struct fat_fs *fs)
{
    return initialized && fs && fs->slot < USB_VOLUME_COUNT ? 0 : -1;
}

int os_integration_init(void)
{
    struct nucleus_mount *base;
    const struct usb_os_profile *profile;
    uint32_t findmount_address;
    volatile uint32_t *entry;
    unsigned i;
    int mask;
    /* Reject other variants before making any OS calls through private addresses. */
    if (usb_os_select(nl_osid())) {
        return -1;
    }
    profile = usb_os_active_profile();
    findmount_address = profile->hooks[USB_HOOK_FINDMOUNT];
    entry = (volatile uint32_t *)(uintptr_t)findmount_address;
    original_findmount = (void *)(uintptr_t)findmount_address;
    log("usb-storage: checking live A: mount...\n");
    base = original_findmount("A:/");
    if (!base || !base->ops ||
        base->ops->fn[OP_ALLOCNODE] != profile->hooks[USB_HOOK_ALLOCATE_FSNODE] ||
        base->ops->fn[OP_DEALLOCNODE] != profile->hooks[USB_HOOK_DEALLOCATE_FSNODE] ||
        base->ops->fn[OP_NODESTRING] != profile->hooks[USB_HOOK_FSNODE_TO_STRING] ||
        base->ops->fn[OP_RENAME] != profile->hooks[USB_HOOK_RENAME]) {
        log("usb-storage: A: mount/callback check failed\n");
        return -1;
    }
    a_ops = base->ops;
    a_allocate = (void *)a_ops->fn[OP_ALLOCNODE];
    a_deallocate = (void *)a_ops->fn[OP_DEALLOCNODE];
    a_node_string = (void *)a_ops->fn[OP_NODESTRING];
    a_rename = (void *)a_ops->fn[OP_RENAME];
    for (i = 0; i < OP_COUNT; ++i) {
        usb_ops.fn[i] = (uintptr_t)unsupported;
    }
    usb_ops.fn[OP_INIT] = (uintptr_t)success;
    usb_ops.fn[OP_OPEN] = (uintptr_t)fs_open;
    usb_ops.fn[OP_READ] = (uintptr_t)fs_read;
    usb_ops.fn[OP_WRITE] = (uintptr_t)fs_write;
    usb_ops.fn[OP_SEEK] = (uintptr_t)fs_seek;
    usb_ops.fn[OP_CLOSE] = (uintptr_t)fs_close;
    usb_ops.fn[OP_DELETE] = (uintptr_t)fs_unlink;
    usb_ops.fn[OP_UTIME] = (uintptr_t)fs_utime;
    usb_ops.fn[OP_SETATTR] = (uintptr_t)fs_setattr;
    usb_ops.fn[OP_GETATTR] = (uintptr_t)fs_getattr;
    usb_ops.fn[OP_TRUNCATE] = (uintptr_t)fs_truncate;
    usb_ops.fn[OP_FLUSH] = (uintptr_t)fs_flush;
    usb_ops.fn[OP_MKDIR] = (uintptr_t)fs_mkdir;
    usb_ops.fn[OP_RMDIR] = (uintptr_t)fs_unlink;
    usb_ops.fn[OP_RENAME] = (uintptr_t)fs_rename;
    usb_ops.fn[OP_FIRST] = (uintptr_t)fs_first;
    usb_ops.fn[OP_NEXT] = (uintptr_t)fs_next;
    usb_ops.fn[OP_DONE] = (uintptr_t)fs_done;
    usb_ops.fn[OP_ALLOCNODE] = (uintptr_t)allocate_node;
    usb_ops.fn[OP_DEALLOCNODE] = (uintptr_t)deallocate_node;
    usb_ops.fn[OP_NODESTRING] = (uintptr_t)node_string;
    /* Real directories make the mount roots visible in the normal Documents listing. */
    log("usb-storage: preparing USB mount directories...\n");
    for (i = 0; i < USB_VOLUME_COUNT; ++i) {
        char path[40];
        struct stat st;
        snprintf(path, sizeof(path), "/documents/usb%c", 'A' + i);
        if (stat(path, &st)) {
            if (mkdir(path, 0777)) {
                log("usb-storage: mkdir %s failed (errno %d)\n", path, errno);
                return -1;
            }
        } else if (!S_ISDIR(st.st_mode)) {
            log("usb-storage: %s exists but is not a directory\n", path);
            return -1;
        }
        overlays[i] = *base;
        overlays[i].ops = &usb_ops;
        overlays[i].refs = 0;
    }
    /* No failure or unload is allowed after a kernel-owned semaphore is created. */
    log("usb-storage: creating filesystem semaphore...\n");
    if (usb_os_semaphore_create(semaphore, "USBFS   ", 1, 6)) {
        log("usb-storage: semaphore creation failed\n");
        return -1;
    }
    log("usb-storage: installing filesystem hooks...\n");
    /* These two instructions are position independent; the third branches on
     * flags produced by the first, which the literal LDR preserves. */
    trampoline[0] = entry[0];
    trampoline[1] = entry[1];
    trampoline[2] = 0xe51ff004;
    trampoline[3] = findmount_address + 8;
    original_findmount = (void *)trampoline;
    mask = TCT_Local_Control_Interrupts(-1);
    a_ops->fn[OP_ALLOCNODE] = (uintptr_t)allocate_node;
    a_ops->fn[OP_DEALLOCNODE] = (uintptr_t)deallocate_node;
    a_ops->fn[OP_NODESTRING] = (uintptr_t)node_string;
    a_ops->fn[OP_RENAME] = (uintptr_t)a_rename_overlay;
    entry[1] = (uintptr_t)overlay_findmount;
    entry[0] = 0xe51ff004;
    initialized = 1;
    clear_cache();
    TCT_Local_Control_Interrupts(mask);
    return 0;
}
