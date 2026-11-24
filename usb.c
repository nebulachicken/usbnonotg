#include <os.h>
#include "usb_storage.h"
#include "usb_host.h"
#include "otg_host.h"
#include "fat.h"
#include "console.h"
#include "os_runtime.h"
#include <string.h>

extern unsigned int nl_osid(void);

/* device_t offsets confirmed for the target OS's BSD USB stack. */
static struct usb_softc *softc(device_t dev)
{
    const struct usb_os_usb_layout *layout = &usb_os_active_profile()->usb;

    return *(struct usb_softc **)((uintptr_t)dev + layout->device_softc_offset);
}

static struct usb_attach_arg *ivars(device_t dev)
{
    const struct usb_os_usb_layout *layout = &usb_os_active_profile()->usb;

    return *(struct usb_attach_arg **)((uintptr_t)dev + layout->device_ivars_offset);
}

static int ums_match(device_t dev)
{
    struct usb_attach_arg *a = ivars(dev);
    usb_interface_descriptor_t *d;
    if (!a || !a->iface) {
        return UMATCH_NONE;
    }
    d = usbd_get_interface_descriptor(a->iface);
    return d && d->bInterfaceClass == UICLASS_MASS && d->bInterfaceSubClass == UISUBCLASS_SCSI &&
                   d->bInterfaceProtocol == UIPROTO_MASS_BBB
               ? UMATCH_IFACECLASS
               : UMATCH_NONE;
}

static void close_pipes(struct usb_softc *sc)
{
    if (sc->sc_bulk_in) {
        usbd_abort_pipe(sc->sc_bulk_in);
        usbd_close_pipe(sc->sc_bulk_in);
        sc->sc_bulk_in = NULL;
    }
    if (sc->sc_bulk_out) {
        usbd_abort_pipe(sc->sc_bulk_out);
        usbd_close_pipe(sc->sc_bulk_out);
        sc->sc_bulk_out = NULL;
    }
}

/* --- Transport over the OS USB stack (OTG adapter: TI-OS is the host) --- */

static int os_bulk(struct usb_softc *sc, int in, void *buf, uint32_t len, uint32_t timeout_ms)
{
    return usbd_do_bulk_transfer(sc->sc_udev, in ? sc->sc_bulk_in : sc->sc_bulk_out, buf, len,
                                 timeout_ms);
}

static int os_recover(struct usb_softc *sc)
{
    usb_device_request_t req;
    memset(&req, 0, sizeof(req));
    req.bmRequestType = UT_WRITE_CLASS_INTERFACE;
    req.bRequest = 0xff; /* Bulk-Only Mass Storage Reset */
    USETW(req.wIndex, sc->sc_ifaceno);
    if (usbd_do_request(sc->sc_udev, &req, NULL) != USBD_NORMAL_COMPLETION) {
        return -1;
    }
    if (usbd_clear_endpoint_stall(sc->sc_bulk_in) != USBD_NORMAL_COMPLETION ||
        usbd_clear_endpoint_stall(sc->sc_bulk_out) != USBD_NORMAL_COMPLETION) {
        return -1;
    }
    usb_bulk_reset_toggles(sc->sc_udev);
    return 0;
}

static void os_delay(struct usb_softc *sc, unsigned ms) { msleep(ms); }

static const struct usb_transport os_transport = {
    .bulk = os_bulk,
    .reset_recovery = os_recover,
    .delay = os_delay,
    .ready_attempts = 5,
};

static int ums_attach(device_t dev)
{
    struct usb_softc *sc = softc(dev);
    struct usb_attach_arg *a = ivars(dev);
    uint8_t count, i;
    int r = -1;
    if (!sc || !a || !a->iface || !a->device) {
        return -1;
    }
    usb_storage_lock();
    memset(sc, 0, sizeof(*sc));
    sc->tp = &os_transport;
    sc->sc_dev = dev;
    sc->sc_iface = a->iface;
    sc->sc_ifaceno = a->ifaceno;
    sc->sc_udev = a->device;
    if (usbd_endpoint_count(sc->sc_iface, &count) != USBD_NORMAL_COMPLETION) {
        goto fail;
    }
    for (i = 0; i < count; ++i) {
        usb_endpoint_descriptor_t *d = usbd_interface2endpoint_descriptor(sc->sc_iface, i);
        if (!d || UE_GET_XFERTYPE(d->bmAttributes) != UE_BULK) {
            continue;
        }
        if (UE_GET_DIR(d->bEndpointAddress) == UE_DIR_IN) {
            sc->sc_ep_in = d->bEndpointAddress;
        } else {
            sc->sc_ep_out = d->bEndpointAddress;
        }
    }
    if (!sc->sc_ep_in || !sc->sc_ep_out) {
        goto fail;
    }
    if (usbd_open_pipe(sc->sc_iface, sc->sc_ep_in, USBD_EXCLUSIVE_USE, &sc->sc_bulk_in) !=
        USBD_NORMAL_COMPLETION) {
        goto fail;
    }
    if (usbd_open_pipe(sc->sc_iface, sc->sc_ep_out, USBD_EXCLUSIVE_USE, &sc->sc_bulk_out) !=
        USBD_NORMAL_COMPLETION) {
        goto fail;
    }
    usb_bulk_reset_toggles(sc->sc_udev);
    if (usb_storage_scsi_refresh_media(sc)) {
        goto fail;
    }
    r = fat_scan_mbr(sc);
    if (r <= 0) {
        goto fail;
    }
    log("usb-storage: mounted %d FAT volume(s)\n", r);
    usb_storage_unlock();
    return 0;
fail:
    fat_detach(sc);
    sc->sc_media.valid = 0;
    sc->sc_detached = 1;
    close_pipes(sc);
    usb_storage_unlock();
    return -1;
}

static int ums_detach(device_t dev)
{
    struct usb_softc *sc = softc(dev);
    if (!sc) {
        return 0;
    }
    usb_storage_lock();
    sc->sc_detached = 1;
    sc->sc_media.valid = 0;
    fat_detach(sc);
    close_pipes(sc);
    usb_storage_unlock();
    return 0;
}

static int (*methods[])(device_t) = {ums_match, ums_attach, ums_detach, NULL};

/* --- Forced host mode (passive adapter), see otg_host.h --- */

/* Resident: the filesystem hooks keep pointing at it after the launcher exits. */
static struct usb_softc forced_sc;

/* Brings the bus up, enumerates the drive and waits until it is ready. Returns 0,
 * OTG_E_OTG_ADAPTER when TI-OS is already the host (use the OS stack instead), or
 * another nonzero value with msg set. Nothing is installed or left running on
 * failure. */
static int start_forced(char *msg, size_t msg_size)
{
    int rc = otg_host_open();
    if (rc) {
        snprintf(msg, msg_size, "%s.", otg_strerror(rc));
        return rc;
    }
    memset(&forced_sc, 0, sizeof(forced_sc));
    if (usb_host_attach(&forced_sc)) {
        snprintf(msg, msg_size, "Host mode is up, but: %s.", usb_host_last_error());
        otg_host_close();
        return -1;
    }
    if (usb_storage_scsi_refresh_media(&forced_sc)) {
        snprintf(msg, msg_size,
                 "The drive did not become ready, or is not a readable SCSI disk with 512 to 4096 "
                 "byte sectors.");
        otg_host_close();
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    char msg[400];
    const char *program = argc > 0 ? argv[0] : NULL;
    int registration = 0, forced = 0, forced_tried = 0, status = 0;

    if (otg_host_probe() == OTG_PROBE_PASSIVE &&
        show_msgbox_2b("usb-storage",
                       "Force USB host mode?\n\nFor a PASSIVE (non-OTG) adapter with the drive "
                       "already plugged in. The calculator switches on its own 5 V output "
                       "from its battery, so use a drive that powers itself, and keep any "
                       "computer or charger unplugged.\n\nSkip to use the normal OS driver "
                       "(OTG adapter).",
                       "Start", "Skip") == 1) {
        int rc;
        forced_tried = 1;
        /* Do not power the bus on an OS this driver cannot hook. */
        if (usb_os_select(nl_osid())) {
            show_msgbox("usb-storage", "This OS version is not supported. Driver not installed.");
            return 1;
        }
        rc = start_forced(msg, sizeof(msg));
        if (rc == 0) {
            forced = 1;
        } else if (rc != OTG_E_OTG_ADAPTER) {
            otg_log_dump(program);
            show_msgbox("usb-storage", msg);
            return 1;
        } /* else: the ID pin is grounded after all: TI-OS hosts, use its USB stack */
    }

    if (usb_console_begin()) {
        show_msgbox("usb-storage", "Cannot initialize startup console. Driver not installed.");
        if (forced) {
            otg_host_close();
        }
        return 1;
    }
    log("usb-storage: starting\n");
    if (os_integration_init()) {
        log("Initialization failed. Driver not installed.\nPress any key to exit.\n");
        if (forced) {
            otg_host_close();
        }
        if (forced_tried) {
            otg_log_dump(program);
        }
        wait_no_key_pressed();
        wait_key_pressed();
        usb_console_end();
        return 1;
    }
    /* Callbacks and trampolines must survive the launcher's exit. */
    nl_set_resident();

    if (forced) {
        int mounted;
        log("usb-storage: forced host mode: mounting...\n");
        usb_storage_lock();
        mounted = fat_scan_mbr(&forced_sc);
        usb_storage_unlock();
        if (mounted <= 0) {
            log("No FAT12/16/32 volume on an MBR disk. Releasing the USB port.\n");
            forced_sc.sc_detached = 1;
            forced_sc.sc_media.valid = 0;
            usb_storage_lock();
            fat_detach(&forced_sc);
            usb_storage_unlock();
            otg_host_close();
            status = 1;
        } else {
            log("usb-storage: mounted %d FAT volume(s)\n", mounted);
            log("Driver installed. Mount roots: A:/documents/usbA through usbC\n");
            log("5 V stays on while the drive is mounted. Close all files before unplugging.\n");
            log("Press any key to return to Documents.\n");
        }
    } else {
        log("usb-storage: registering USB driver...\n");
        registration = usb_register_driver(2, methods, "usb-storage", 0, sizeof(struct usb_softc));
        if (registration) {
            log("USB registration failed (%d). Reboot before retrying.\n", registration);
            status = 1;
        } else {
            log("Driver installed. Mount roots: A:/documents/usbA through usbC\n");
            log("Press any key to return to Documents, then connect USB storage.\n");
        }
        if (registration) {
            log("Press any key to exit.\n");
        }
    }
    if (forced_tried) {
        otg_log_dump(program);
    }
    wait_no_key_pressed();
    wait_key_pressed();
    usb_console_end();
    /* Hooks already point into this program even if mounting or registration failed. */
    _exit(status);
}
