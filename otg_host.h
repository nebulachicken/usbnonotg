#ifndef OTG_HOST_H
#define OTG_HOST_H

/* Forced USB host mode for the TI-Nspire CX II top (mini-USB) port.
 *
 * Lets a PASSIVE adapter (ID pin floating) act as host: the calculator switches
 * its own 5 V on, arms HNP so the controller flips to host role, resets the port
 * and then drives control and bulk transfers itself by polling the FOTG210
 * controller. Ported from otgweb.c; the register sequences are unchanged.
 *
 * Locking: every function here must be called by one task at a time. After the
 * filesystem hooks are installed that is guaranteed by usb_storage_lock().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One bulk chunk is one qTD on one 4 KiB page. */
#define OTG_BULK_CHUNK 4096u

enum otg_probe {
    OTG_PROBE_PASSIVE = 0,   /* ID floating (or controller not readable yet) */
    OTG_PROBE_OTG_ADAPTER,   /* ID grounded: TI-OS is already the host */
    OTG_PROBE_POWER_PRESENT, /* computer, charger or powered hub on VBUS */
    OTG_PROBE_CHARGER_ON     /* TI-OS has not switched its charger off yet */
};

enum otg_error {
    OTG_OK = 0,
    OTG_E_BUSY = -1,
    OTG_E_ENV = -2, /* cannot mask interrupts, or the reference timer does not count */
    OTG_E_POWER = -3,
    OTG_E_OTG_ADAPTER = -4,
    OTG_E_POWER_PRESENT = -5,
    OTG_E_CHARGER = -6,
    OTG_E_NO_VBUS = -7,
    OTG_E_FAULT = -8, /* over-current or VBUS error */
    OTG_E_NO_HOST = -9,
    OTG_E_NO_DEVICE = -10,
    OTG_E_NOT_ENABLED = -11
};

/* Results of otg_bulk_chunk() below zero. */
#define OTG_XFER_ERROR (-1)
#define OTG_XFER_STALL (-2)
#define OTG_XFER_TIMEOUT (-3)

/* Read-only look at the ID pin, VBUS and charger. Never changes hardware state. */
int otg_host_probe(void);

/* Full bring-up: 5 V on, HNP, role switch, port reset. On success the session
 * stays open (5 V on, controller in host role, device reset and enabled) until
 * otg_host_close() or until a later otg_op_begin() finds it lost. On failure
 * everything is restored and an OTG_E_* code is returned. */
int otg_host_open(void);
void otg_host_close(void);
bool otg_host_active(void);
/* Called by the optional background worker while holding the storage lock.
 * Returns 0 when healthy, 1 for a transient inability to enter, or -1
 * when the session has been torn down.
 */
int otg_host_background_tick(void);

/* Bracket every burst of hardware access. Takes over the timer and the USB
 * register window, re-checks that the session is still alive and lets TI-OS run
 * meanwhile (undoing what it does to the USB block). Calls nest. Returns 0, or
 * -1 when the session is gone (it has been closed and 5 V switched off). */
int otg_op_begin(void);
void otg_op_end(void);

/* Valid only between otg_op_begin() and otg_op_end(). */
uint32_t otg_now_ms(void);
void otg_wait_ms(uint32_t ms);

unsigned otg_host_speed(void); /* 0 full, 1 low, 2 high */
void otg_host_set_device(unsigned addr, unsigned ep0_mps);

/* One control transfer on endpoint 0 of the current device. IN data is copied to
 * `in` (at most `length` bytes). Returns the bytes moved or -1. */
int otg_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, const void *out,
                void *in, uint16_t length);

/* One bulk transfer of at most OTG_BULK_CHUNK bytes. `toggle` is the DATA0/1
 * state to start with. Returns the bytes moved (IN: may be short), or one of
 * OTG_XFER_*. The caller tracks the toggle (see usb_host_packets()). */
int otg_bulk_chunk(uint8_t ep, bool in, void *buf, unsigned len, unsigned mps, unsigned toggle,
                   uint32_t timeout_ms);

/* In-RAM log of the bring-up (and later faults), written to a file by main(). */
void otg_log(const char *format, ...) __attribute__((format(printf, 1, 2)));
size_t otg_log_dump(const char *program_path);

const char *otg_strerror(int code);

#endif
