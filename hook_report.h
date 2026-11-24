#ifndef USB_HOOK_REPORT_H
#define USB_HOOK_REPORT_H

#include "hook_patterns.h"

typedef void (*usb_hook_log_fn)(void *context, const char *line);

void usb_hook_report(const struct usb_hook_reader *reader, const struct usb_os_profile *profile,
                     const struct usb_hook_matches matches[USB_HOOK_COUNT], usb_hook_log_fn emit,
                     void *context);

#endif
