#ifndef USB_BACKGROUND_H
#define USB_BACKGROUND_H

/* Optional resident Nucleus maintenance worker. Returns 0 when started,
 * 1 when task creation is unavailable for this OS, or -1 on startup error.
 */
int usb_background_start(void);
int usb_background_is_running(void);

#endif
