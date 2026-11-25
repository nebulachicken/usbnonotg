/* Enumeration and Bulk-Only transport for a device on the forced-host session.
 * The control-transfer sequence follows otgweb.c's read_device(), with the CDC
 * endpoint search replaced by a Bulk-Only SCSI interface search. */

#include <string.h>
#include "otg_host.h"
#include "usb_host.h"

#define CFG_MAX 512

static const char *last_error = "";

const char *usb_host_last_error(void) { return last_error; }

static int fail(const char *why)
{
    last_error = why;
    otg_log("attach: %s", why);
    return -1;
}

int usb_host_parse_config(const uint8_t *d, size_t n, struct usb_host_dev *out)
{
    struct usb_host_dev cur;
    int cur_ok = 0;
    size_t i;
    if (!d || !out || n < 9 || d[1] != 2) {
        return 0;
    }
    memset(&cur, 0, sizeof cur);
    for (i = 0; i + 2 <= n;) {
        size_t len = d[i];
        unsigned type = d[i + 1];
        if (len < 2 || i + len > n) {
            break; /* malformed: stop rather than read past the block */
        }
        if (type == 4 && len >= 9) {
            if (cur_ok && cur.ep_in && cur.ep_out) {
                break; /* the matching interface is complete */
            }
            /* alternate setting 0, class 8 (mass storage), subclass 6 (SCSI),
             * protocol 0x50 (Bulk-Only) */
            cur_ok = d[i + 3] == 0 && d[i + 5] == 8 && d[i + 6] == 6 && d[i + 7] == 0x50;
            memset(&cur, 0, sizeof cur);
            cur.cfg = d[5];
            cur.iface = d[i + 2];
        } else if (type == 5 && len >= 7 && cur_ok && (d[i + 3] & 3) == 2) {
            unsigned mps = (unsigned)(d[i + 4] | d[i + 5] << 8) & 0x7ff;
            if (d[i + 2] & 0x80) {
                if (!cur.ep_in) {
                    cur.ep_in = d[i + 2];
                    cur.mps_in = (uint16_t)mps;
                }
            } else if (!cur.ep_out) {
                cur.ep_out = d[i + 2];
                cur.mps_out = (uint16_t)mps;
            }
        }
        i += len;
    }
    if (cur_ok && cur.ep_in && cur.ep_out && cur.mps_in && cur.mps_out) {
        *out = cur;
        return 1;
    }
    return 0;
}

unsigned usb_host_packets(int in, unsigned moved, unsigned requested, unsigned mps)
{
    if (!mps) {
        return 0;
    }
    if (in) {
        return moved / mps + ((moved % mps || moved < requested) ? 1 : 0);
    }
    return moved ? (moved + mps - 1) / mps : 1;
}

/* ------------------------------------------------------------ transport */

#define MAX_RECONNECT_FAILURES 2

static uint32_t fnv1a(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) {
        h = (h ^ *p++) * 16777619u;
    }
    return h;
}

/* Capacity and a hash of sector 0 (MBR / boot sector, including the disk
 * signature). Read through the SCSI layer, so it needs a live session. */
static int media_identity(struct usb_softc *sc, uint32_t *hash)
{
    static uint8_t sector[4096];
    if (sc->sc_media.block_size == 0 || sc->sc_media.block_size > sizeof sector) {
        return -1;
    }
    if (usb_storage_scsi_blocks(sc, 0, 1, sector, sc->sc_media.block_size, 0)) {
        return -1;
    }
    *hash = fnv1a(sector, sc->sc_media.block_size);
    return 0;
}

int usb_host_remember_media(struct usb_softc *sc)
{
    uint32_t hash;
    if (!sc || !sc->sc_media.valid || media_identity(sc, &hash)) {
        return -1;
    }
    sc->host.media_hash = hash;
    sc->host.media_last_lba = sc->sc_media.last_lba;
    sc->host.media_block = sc->sc_media.block_size;
    sc->host.media_known = 1;
    return 0;
}

/* The session is gone or unusable (TI-OS got at the controller, the drive
 * dropped off, a recovery failed). Bring the bus up again and make sure it is
 * the same drive, so the filesystem state kept in RAM is still valid. Must run
 * outside any hardware burst (the caller is host_enter()). */
static int host_reconnect(struct usb_softc *sc)
{
    struct usb_host_dev *h = &sc->host;
    uint32_t hash = 0;
    int rc;
    if (h->gave_up || h->reconnecting) {
        return -1;
    }
    h->reconnecting = 1;
    h->need_reconnect = 0;
    otg_log("reconnect: bringing the bus up again (attempt %u)", (unsigned)h->failures + 1);
    otg_host_close(); /* no-op when the session is already closed */
    rc = otg_host_open();
    if (rc) {
        otg_log("reconnect: %s", otg_strerror(rc));
        goto failed;
    }
    if (usb_host_attach(sc)) {
        otg_log("reconnect: attach failed: %s", usb_host_last_error());
        goto failed_open;
    }
    if (usb_storage_scsi_refresh_media(sc)) {
        otg_log("reconnect: the drive did not become ready");
        goto failed_open;
    }
    if (h->media_known) {
        if (sc->sc_media.last_lba != h->media_last_lba ||
            sc->sc_media.block_size != h->media_block || media_identity(sc, &hash) ||
            hash != h->media_hash) {
            /* A different drive: the cached FAT state would corrupt it. */
            otg_log("reconnect: this is a different drive, not remounting");
            h->gave_up = 1;
            h->reconnecting = 0;
            otg_host_close();
            return -1;
        }
    }
    h->failures = 0;
    h->bad_streak = 0;
    h->reconnecting = 0;
    otg_log("reconnect: ok, same drive again");
    return 0;
failed_open:
    otg_host_close();
failed:
    if (++h->failures >= MAX_RECONNECT_FAILURES) {
        h->gave_up = 1;
        otg_log("reconnect: giving up, the drive is gone");
    }
    h->reconnecting = 0;
    return -1;
}

static int host_enter(struct usb_softc *sc)
{
    struct usb_host_dev *h = &sc->host;
    if (!h->need_reconnect && !otg_op_begin()) {
        return 0;
    }
    /* Not from inside a hardware burst or from the reconnect's own commands. */
    if (otg_in_op() || h->reconnecting) {
        return 1;
    }
    if (host_reconnect(sc)) {
        return h->gave_up ? -1 : 1; /* a failed attempt may be followed by a working one */
    }
    return otg_op_begin() ? 1 : 0;
}

static void host_leave(struct usb_softc *sc)
{
    (void)sc;
    otg_op_end();
}

static void host_delay(struct usb_softc *sc, unsigned ms)
{
    (void)sc;
    if (!otg_op_begin()) {
        otg_wait_ms(ms);
        otg_op_end();
    }
}

static int host_bulk(struct usb_softc *sc, int in, void *buf, uint32_t len, uint32_t timeout_ms)
{
    uint8_t *p = buf;
    uint8_t ep = in ? sc->host.ep_in : sc->host.ep_out;
    unsigned mps = in ? sc->host.mps_in : sc->host.mps_out;
    uint8_t *toggle = in ? &sc->host.toggle_in : &sc->host.toggle_out;
    uint32_t total = 0, t0 = otg_now_ms();
    if (!buf || !len) {
        return -1;
    }
    while (total < len) {
        unsigned chunk = len - total > OTG_BULK_CHUNK ? OTG_BULK_CHUNK : (unsigned)(len - total);
        uint32_t elapsed = otg_now_ms() - t0;
        int r;
        if (elapsed >= timeout_ms) {
            return -1;
        }
        r = otg_bulk_chunk(ep, in != 0, p + total, chunk, mps, *toggle, timeout_ms - elapsed);
        if (r < 0) {
            return r;
        }
        *toggle ^= (uint8_t)(usb_host_packets(in, (unsigned)r, chunk, mps) & 1u);
        total += (uint32_t)r;
        if ((unsigned)r < chunk) {
            break; /* short transfer: the device has nothing more to send */
        }
    }
    sc->host.bad_streak = 0; /* the bus works */
    return (int)total;
}

static int clear_halt(uint8_t ep) { return otg_control(0x02, 1, 0, ep, NULL, NULL, 0) < 0 ? -1 : 0; }

/* A failed command normally just needs a Bulk-Only reset. When that does not
 * work either, or keeps being needed without any transfer succeeding in
 * between, the session is broken: flag it for repair before the next command
 * and report success, so the SCSI layer keeps the medium valid instead of
 * declaring it dead for good. (The failed command itself still returns an
 * error to its caller.) */
static int host_reset_recovery(struct usb_softc *sc)
{
    struct usb_host_dev *h = &sc->host;
    int ok = otg_control(0x21, 0xff, 0, h->iface, NULL, NULL, 0) >= 0 && !clear_halt(h->ep_in) &&
             !clear_halt(h->ep_out);
    h->toggle_in = h->toggle_out = 0;
    if (!ok || ++h->bad_streak >= 3) {
        otg_log("recovery %s: session will be re-established", ok ? "keeps being needed" : "failed");
        h->need_reconnect = 1;
    }
    return 0;
}

/* Spinning drives need seconds to become ready: poll for 10 s. */
const struct usb_transport usb_host_transport = {
    .enter = host_enter,
    .leave = host_leave,
    .bulk = host_bulk,
    .reset_recovery = host_reset_recovery,
    .delay = host_delay,
    .ready_attempts = 100,
};

/* ----------------------------------------------------------- enumeration */

int usb_host_attach(struct usb_softc *sc)
{
    static uint8_t cfgbuf[CFG_MAX];
    uint8_t dev[18], hdr[9];
    struct usb_host_dev found;
    unsigned speed, ep0_mps, nconf, c;
    int n, rc = -1, ok = 0;

    memset(&found, 0, sizeof found);
    if (otg_op_begin()) {
        return fail("the host session is not active");
    }
    speed = otg_host_speed();
    if (speed == 1) {
        rc = fail("low-speed device (no bulk endpoints)");
        goto out;
    }
    ep0_mps = speed == 2 ? 64 : 8;
    otg_host_set_device(0, ep0_mps);
    n = otg_control(0x80, 6, 0x0100, 0, NULL, dev, 8);
    if (n < 8) {
        rc = fail("could not read the device descriptor");
        goto out;
    }
    if (dev[7]) {
        ep0_mps = dev[7];
        otg_host_set_device(0, ep0_mps);
    }
    n = otg_control(0x80, 6, 0x0100, 0, NULL, dev, 18);
    if (n < 18) {
        rc = fail("could not read the full device descriptor");
        goto out;
    }
    otg_log("device: usb=%x.%02x class=%02x mps0=%u vid=%04x pid=%04x configs=%u speed=%u", dev[3],
            dev[2], dev[4], dev[7], dev[8] | dev[9] << 8, dev[10] | dev[11] << 8, dev[17], speed);
    if (otg_control(0x00, 5, 1, 0, NULL, NULL, 0) < 0) {
        rc = fail("SET_ADDRESS failed");
        goto out;
    }
    otg_wait_ms(10);
    otg_host_set_device(1, ep0_mps);

    nconf = dev[17] > 4 ? 4 : dev[17];
    for (c = 0; c < nconf && !ok; ++c) {
        unsigned total;
        n = otg_control(0x80, 6, (uint16_t)(0x0200 | c), 0, NULL, hdr, 9);
        if (n < 9) {
            continue;
        }
        total = (unsigned)(hdr[2] | hdr[3] << 8);
        if (total > sizeof cfgbuf) {
            total = sizeof cfgbuf;
        }
        n = otg_control(0x80, 6, (uint16_t)(0x0200 | c), 0, NULL, cfgbuf, (uint16_t)total);
        if (n < 9) {
            continue;
        }
        ok = usb_host_parse_config(cfgbuf, (size_t)n, &found);
    }
    if (!ok) {
        rc = fail("no Bulk-Only mass-storage interface (not a USB drive?)");
        goto out;
    }
    otg_log("chosen: cfg %u, interface %u, IN ep %02x (%u), OUT ep %02x (%u)", found.cfg,
            found.iface, found.ep_in, found.mps_in, found.ep_out, found.mps_out);
    if (otg_control(0x00, 9, found.cfg, 0, NULL, NULL, 0) < 0) {
        rc = fail("SET_CONFIGURATION failed");
        goto out;
    }
    otg_wait_ms(20);

    sc->host.cfg = found.cfg;
    sc->host.iface = found.iface;
    sc->host.ep_in = found.ep_in;
    sc->host.ep_out = found.ep_out;
    sc->host.mps_in = found.mps_in;
    sc->host.mps_out = found.mps_out;
    sc->host.toggle_in = sc->host.toggle_out = 0;
    sc->tp = &usb_host_transport;
    sc->sc_ifaceno = found.iface;
    sc->sc_detached = 0;
    rc = 0;
out:
    otg_op_end();
    return rc;
}
