#ifndef USB_OS_RUNTIME_H
#define USB_OS_RUNTIME_H

#include "os_profiles.h"

/* Selection validates a reviewed profile and every private-function pattern.
 * Pending profiles are available to the scanner but cannot install the driver.
 */
int usb_os_select(unsigned os_id);
const struct usb_os_profile *usb_os_active_profile(void);
int usb_os_parse_drive(const char *path, int allow_current);
int usb_os_semaphore_create(void *semaphore, const char *name, unsigned count, unsigned order);
int usb_os_semaphore_obtain(void *semaphore, unsigned timeout);
int usb_os_semaphore_release(void *semaphore);

#endif
