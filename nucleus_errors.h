#ifndef USB_NUCLEUS_ERRORS_H
#define USB_NUCLEUS_ERRORS_H

/* Values understood by the target Nucleus FILE/libc adapter. */
enum nucleus_file_error {
    NUF_BAD_PARAMETER = -3002,
    NUF_NAME_TOO_LONG = -3017,
    NUF_TOO_MANY_FILES = -3019,
    NUF_BAD_DESCRIPTOR = -3020,
    NUF_ACCESS_DENIED = -3021,
    NUF_NO_SPACE = -3022,
    NUF_NO_FILE = -3024,
    NUF_EXISTS = -3025,
    NUF_INVALID_PARAMETER = -3030,
    NUF_IO_ERROR = -3036
};

enum nucleus_open_flag {
    NUF_OPEN_READ = 0,
    NUF_OPEN_WRITE = 1,
    NUF_OPEN_READWRITE = 2,
    NUF_OPEN_ACCESS_MASK = 3,
    NUF_OPEN_APPEND = 0x008,
    NUF_OPEN_CREATE = 0x100,
    NUF_OPEN_TRUNCATE = 0x200,
    NUF_OPEN_EXCLUSIVE = 0x400
};

#endif
