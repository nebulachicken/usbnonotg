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

/* Experimental Nucleus task support. It is disabled unless the active
 * OS profile contains a verified function address and code signature.
 */
typedef void (*usb_os_task_entry)(unsigned argc, void *argv);
int usb_os_background_task_available(void);
int usb_os_task_create(void *tcb, char *name, usb_os_task_entry entry,
                       unsigned argc, void *argv, void *stack,
                       unsigned stack_size, unsigned priority,
                       unsigned time_slice, unsigned preempt,
                       unsigned auto_start);

#endif
