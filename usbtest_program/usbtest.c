/* Run after installing the resident driver and inserting a FAT USB device.
 * Uses normal libc calls only. Refuses to overwrite an existing test directory.
 */
#include <os.h>
#include <nspireio2.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static nio_console console;
#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            nio_printf(&console, "FAIL line %d: %s (errno %d)\n", __LINE__, #expr, errno);         \
            goto done;                                                                             \
        }                                                                                          \
    } while (0)

int main(void)
{
    FILE *f = NULL;
    DIR *dir = NULL;
    struct stat st;
    char data[16], oldcwd[256];
    int close_result;
    const char *root = "A:/documents/usbA/.usb-storage-test";
    int changed_cwd = 0;
    nio_InitConsole(&console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, 15, 0);
    CHECK(getcwd(oldcwd, sizeof(oldcwd)) != NULL);
    CHECK(stat("A:/documents/usbA", &st) == 0 && S_ISDIR(st.st_mode));
    CHECK(stat(root, &st) != 0); /* Existing user content must be preserved. */
    CHECK(mkdir(root, 0777) == 0);
    CHECK(chdir(root) == 0);
    changed_cwd = 1;
    CHECK(getcwd(data, sizeof(data)) == NULL); /* Small-buffer getcwd must fail. */
    f = fopen("check.txt", "w+");
    CHECK(f != NULL);
    CHECK(fwrite("USB", 1, 3, f) == 3);
    CHECK(fseek(f, 8, SEEK_SET) == 0 && ftell(f) == 8);
    CHECK(fwrite("!", 1, 1, f) == 1 && fflush(f) == 0);
    CHECK(fseek(f, 0, SEEK_SET) == 0);
    memset(data, 0xff, sizeof(data));
    CHECK(fread(data, 1, 9, f) == 9);
    CHECK(memcmp(data, "USB\0\0\0\0\0!", 9) == 0);
    close_result = fclose(f);
    f = NULL;
    CHECK(close_result == 0);
    CHECK(stat("check.txt", &st) == 0 && st.st_size == 9);
    CHECK(rename("check.txt", "renamed.txt") == 0);
    f = fopen("renamed.txt", "a");
    CHECK(f != NULL);
    CHECK(fwrite("+", 1, 1, f) == 1);
    close_result = fclose(f);
    f = NULL;
    CHECK(close_result == 0);
    dir = opendir(".");
    CHECK(dir != NULL);
    CHECK(readdir(dir) != NULL);
    CHECK(closedir(dir) == 0);
    dir = NULL;
    CHECK(remove("renamed.txt") == 0);
    CHECK(chdir(oldcwd) == 0);
    changed_cwd = 0;
    CHECK(rmdir(root) == 0);
    nio_printf(&console,
               "PASS: stdio, seek/gap, stat, rename, append, cwd, directory and removal\n");
done:
    if (f) {
        fclose(f);
    }
    if (dir) {
        closedir(dir);
    }
    if (changed_cwd) {
        chdir(oldcwd);
    }
    nio_printf(&console, "Press any key to exit.\n");
    nio_DrawConsole(&console);
    wait_key_pressed();
    return 0;
}
