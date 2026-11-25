#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include <stddef.h>
#include <stdint.h>
#include <usb.h>
#include <usbdi.h>

struct fat_fs;
struct usb_softc;

/* How bulk data and recovery reach the device. The OS USB stack path (OTG
 * adapter) and the forced-host path (passive adapter, see otg_host.h) each
 * provide one; the SCSI layer does not care which. */
struct usb_transport {
    /* Bracket every SCSI command; both may be NULL. enter() returns 0 to
     * proceed, a positive value when only this command has to fail (the device
     * may come back), or a negative value when the device is gone for good (it
     * is then detached). */
    int (*enter)(struct usb_softc *sc);
    void (*leave)(struct usb_softc *sc);
    /* Move len bytes on the bulk-IN (in != 0) or bulk-OUT endpoint. Returns the
     * bytes moved (IN may be short) or a negative value. */
    int (*bulk)(struct usb_softc *sc, int in, void *buf, uint32_t len, uint32_t timeout_ms);
    /* Bulk-Only reset recovery. Returns 0 on success. */
    int (*reset_recovery)(struct usb_softc *sc);
    void (*delay)(struct usb_softc *sc, unsigned ms);
    /* How many 100 ms TEST UNIT READY polls to allow (spinning drives are slow). */
    unsigned ready_attempts;
};

/* Mass-storage interface found by the forced-host enumeration. */
struct usb_host_dev {
    uint8_t cfg;
    uint8_t iface;
    uint8_t ep_in;
    uint8_t ep_out;
    uint16_t mps_in;
    uint16_t mps_out;
    uint8_t toggle_in;
    uint8_t toggle_out;
    /* Reconnect state (survives re-enumeration). */
    uint8_t need_reconnect; /* repair the session before the next command */
    uint8_t reconnecting;
    uint8_t gave_up; /* the drive is gone for good: stop trying */
    uint8_t failures; /* consecutive failed reconnects */
    uint8_t bad_streak; /* recoveries without a successful transfer in between */
    uint8_t media_known;
    uint32_t media_hash; /* identity of the drive as first mounted */
    uint32_t media_last_lba;
    uint32_t media_block;
};

struct usb_storage_media_info {
    uint32_t last_lba;
    uint32_t block_size;
    uint64_t capacity_bytes;
    uint8_t valid;
    uint8_t write_protected;
};

struct usb_softc {
    device_t sc_dev;
    usbd_interface_handle sc_iface;
    int sc_ifaceno;
    usbd_device_handle sc_udev;
    uint8_t sc_ep_in;
    uint8_t sc_ep_out;
    usbd_pipe_handle sc_bulk_in;
    usbd_pipe_handle sc_bulk_out;
    struct usb_storage_media_info sc_media;
    uint32_t sc_tag;
    uint8_t sc_detached;
    const struct usb_transport *tp;
    struct usb_host_dev host; /* forced-host transport only */
};

struct bbb_cbw {
    uint32_t dCBWSignature;
    uint32_t dCBWTag;
    uint32_t dCBWDataTransferLength;
    uint8_t bmCBWFlags;
    uint8_t bCBWLUN;
    uint8_t bCBWCBLength;
    uint8_t CBWCB[16];
} __attribute__((packed));

struct bbb_csw {
    uint32_t dCSWSignature;
    uint32_t dCSWTag;
    uint32_t dCSWDataResidue;
    uint8_t bCSWStatus;
} __attribute__((packed));

struct scsi_read_capacity_10_data {
    uint32_t returned_lba;
    uint32_t block_length;
} __attribute__((packed));

int usbd_do_bulk_transfer(usbd_device_handle dev, usbd_pipe_handle ep, void *buf, uint32_t len,
                          uint32_t timeout_ms);
void usb_bulk_reset_toggles(usbd_device_handle dev);
int usb_storage_scsi_refresh_media(struct usb_softc *sc);
int usb_storage_scsi_read_sectors(struct usb_softc *sc, uint32_t lba, uint16_t sector_count,
                                  void *buf, size_t buf_len);
int usb_storage_scsi_blocks(struct usb_softc *sc, uint32_t lba, uint16_t count, void *buf,
                            size_t len, int write);
int usb_storage_scsi_sync(struct usb_softc *sc);
int os_integration_init(void);
int os_integration_mount(struct fat_fs *fs);
void usb_storage_lock(void);
void usb_storage_unlock(void);

#endif
