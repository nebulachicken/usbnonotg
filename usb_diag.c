#include <stdbool.h>
#include "fat.h"
#include "otg_host.h"
#include "usb_diag.h"

#define LOG_NAME "usb-storage-log.txt"
#define MAX_FLUSHES 40 /* a safety net against a log that keeps being rewritten */

void usb_diag_flush(void)
{
    static bool busy;
    static unsigned flushes;
    struct fat_fs *fs;
    const char *text;
    size_t length;
    unsigned i;
    if (busy || flushes >= MAX_FLUSHES || !otg_log_pending()) {
        return;
    }
    busy = true;
    otg_log_clear_pending();
    fs = fat_get_mounted_filesystems();
    for (i = 0; i < FAT_MAX_MOUNTED_FILESYSTEMS; ++i) {
        struct fat_stat st;
        int fd;
        if (!fs[i].valid || !fs[i].sc || fs[i].sc->sc_detached || !fs[i].sc->sc_media.valid ||
            fs[i].sc->sc_media.write_protected) {
            continue;
        }
        /* Only a drive where the user created the file. Never create it here. */
        if (fat_stat_path(&fs[i], LOG_NAME, &st) || (st.attributes & AM_DIR)) {
            continue;
        }
        fd = fat_open_flags(&fs[i], LOG_NAME, NUF_OPEN_WRITE | NUF_OPEN_TRUNCATE);
        if (fd > 0) {
            /* The log must not change while it is being copied out. */
            otg_log_freeze(true);
            text = otg_log_buffer(&length);
            fat_write(fd, text, length);
            otg_log_freeze(false);
            fat_close(fd);
            ++flushes;
        }
        break;
    }
    /* Events caused by writing the log must not trigger another write. */
    otg_log_clear_pending();
    busy = false;
}
