#include "namespace.h"
#include <string.h>

static int sep(char c)
{
    return c == '/' || c == '\\';
}

static char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

int usb_path_normalize(const char *path, const char *cwd, char out[USB_PATH_MAX])
{
    char input[USB_PATH_MAX];
    size_t n = 0, i = 0, used = 3;
    if (!path || !*path) {
        return -1;
    }
    if (path[1] == ':') {
        if (lower(path[0]) != 'a') {
            return -1;
        }
        path += 2;
    }
    if (!sep(*path)) {
        if (!cwd || !*cwd) {
            return -1;
        }
        if (cwd[1] == ':') {
            if (lower(cwd[0]) != 'a') {
                return -1;
            }
            cwd += 2;
        }
        n = strlen(cwd);
        if (n + 2 >= sizeof(input)) {
            return -1;
        }
        memcpy(input, cwd, n);
        input[n++] = '/';
    }
    if (strlen(path) >= sizeof(input) - n) {
        return -1;
    }
    strcpy(input + n, path);
    memcpy(out, "A:/", 4);
    while (input[i]) {
        size_t start, len;
        while (sep(input[i])) {
            ++i;
        }
        start = i;
        while (input[i] && !sep(input[i])) {
            ++i;
        }
        len = i - start;
        if (!len || (len == 1 && input[start] == '.')) {
            continue;
        }
        if (len == 2 && input[start] == '.' && input[start + 1] == '.') {
            if (used > 3) {
                while (used > 3 && out[used - 1] != '/') {
                    --used;
                }
                if (used > 3) {
                    --used;
                }
            }
            continue;
        }
        if (len > 255 || used + len + 2 > USB_PATH_MAX) {
            return -1;
        }
        if (used > 3) {
            out[used++] = '/';
        }
        memcpy(out + used, input + start, len);
        used += len;
    }
    out[used] = 0;
    return 0;
}

int usb_path_volume(const char *path, const char **relative)
{
    static const char prefix[] = "A:/documents/usb";
    size_t i;
    int v;
    if (!path) {
        return -1;
    }
    for (i = 0; prefix[i]; ++i) {
        if (lower(path[i]) != lower(prefix[i])) {
            return -1;
        }
    }
    v = lower(path[i]) - 'a';
    if (v < 0 || v >= USB_VOLUME_COUNT || (path[i + 1] && path[i + 1] != '/')) {
        return -1;
    }
    if (relative) {
        *relative = path[i + 1] ? path + i + 2 : path + i + 1;
    }
    return v;
}
