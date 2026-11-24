#ifndef USB_NAMESPACE_H
#define USB_NAMESPACE_H
#include <stddef.h>
#define USB_PATH_MAX 512
#define USB_VOLUME_COUNT 3
int usb_path_normalize(const char *path, const char *cwd, char out[USB_PATH_MAX]);
int usb_path_volume(const char *normalized, const char **relative);
#endif
