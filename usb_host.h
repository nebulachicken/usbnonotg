#ifndef USB_HOST_H
#define USB_HOST_H

#include <stddef.h>
#include <stdint.h>
#include "usb_storage.h"

/* Transport for a device on the forced-host session (otg_host.h). */
extern const struct usb_transport usb_host_transport;

/* Enumerate the device on the open session: descriptors, SET_ADDRESS, pick the
 * Bulk-Only SCSI interface, SET_CONFIGURATION. Fills sc->host and sc->tp.
 * Returns 0, or -1 with usb_host_last_error() describing why. */
int usb_host_attach(struct usb_softc *sc);
const char *usb_host_last_error(void);

/* Remember what drive this is (capacity and a hash of sector 0), so that a
 * reconnect can refuse a different drive. Call once after the first
 * usb_storage_scsi_refresh_media(). Returns 0 on success. */
int usb_host_remember_media(struct usb_softc *sc);

/* Find a Bulk-Only (class 8, subclass 6, protocol 0x50) interface with a bulk-IN
 * and a bulk-OUT endpoint in a configuration descriptor block. Returns 1 and
 * fills the usb_host_dev fields when found, otherwise 0. Never reads past n bytes. */
int usb_host_parse_config(const uint8_t *descriptors, size_t n, struct usb_host_dev *out);

/* Number of bus packets a transfer moved, which decides how the DATA0/DATA1
 * toggle advances. For IN, `moved` is what arrived out of `requested`; a short
 * transfer ended with a short or zero-length packet, which is a packet too. */
unsigned usb_host_packets(int in, unsigned moved, unsigned requested, unsigned mps);

#endif
