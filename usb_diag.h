#ifndef USB_DIAG_H
#define USB_DIAG_H

/* Opt-in event log on the drive itself.
 *
 * If a file named usb-storage-log.txt already exists in the root of a mounted
 * drive, the driver's event log (see otg_host.h) is written into it whenever
 * something has happened since the last write: a lost session, a reconnect, a
 * failed transfer. Create the empty file on a PC to turn this on. Nothing is
 * ever written to a drive that does not have that file.
 *
 * Called with the storage lock held and no FatFs call in progress (from the
 * outermost usb_storage_unlock()), never from inside a disk transfer. */
void usb_diag_flush(void);

#endif
