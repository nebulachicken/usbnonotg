#include "usb_storage.h"
#include <string.h>

static int recover(struct usb_softc *sc)
{
    return sc->tp && sc->tp->reset_recovery ? sc->tp->reset_recovery(sc) : -1;
}

static int bulk(struct usb_softc *sc, int in, void *buf, uint32_t len, uint32_t timeout_ms)
{
    return sc->tp->bulk(sc, in, buf, len, timeout_ms);
}

static void delay(struct usb_softc *sc, unsigned ms)
{
    if (sc->tp->delay) {
        sc->tp->delay(sc, ms);
    }
}

/* 0 success, 1 SCSI command failure, -1 transport/protocol failure. */
static int command_locked(struct usb_softc *sc, const uint8_t *cdb, uint8_t cdb_len, void *buf,
                          uint32_t len, int in)
{
    struct bbb_cbw cbw;
    struct bbb_csw csw;
    int done;
    if (!sc || !sc->tp || sc->sc_detached || !cdb_len || cdb_len > 16 || len > 16384) {
        return -1;
    }
    memset(&cbw, 0, sizeof(cbw));
    memset(&csw, 0, sizeof(csw));
    cbw.dCBWSignature = 0x43425355;
    cbw.dCBWTag = ++sc->sc_tag;
    cbw.dCBWDataTransferLength = len;
    cbw.bmCBWFlags = in ? 0x80 : 0;
    cbw.bCBWCBLength = cdb_len;
    memcpy(cbw.CBWCB, cdb, cdb_len);
    if (bulk(sc, 0, &cbw, sizeof(cbw), 1000) != (int)sizeof(cbw)) {
        goto bad;
    }
    done = len ? bulk(sc, in, buf, len, 1000) : 0;
    if (done < 0) {
        goto bad;
    }
    if (bulk(sc, 1, &csw, sizeof(csw), 1000) != (int)sizeof(csw)) {
        goto bad;
    }
    if (csw.dCSWSignature != 0x53425355 || csw.dCSWTag != cbw.dCBWTag ||
        csw.dCSWDataResidue > len || csw.bCSWStatus > 1) {
        goto bad;
    }
    if (csw.bCSWStatus == 1) {
        return 1;
    }
    if ((uint32_t)done != len || csw.dCSWDataResidue) {
        goto bad;
    }
    return 0;
bad:
    if (recover(sc)) {
        sc->sc_media.valid = 0;
    }
    return -1; /* Never retry a potentially partially completed WRITE. */
}

/* Brackets the command with the transport's enter/leave (the forced-host path
 * takes over the controller there). A device that is gone ends up detached. */
static int command(struct usb_softc *sc, const uint8_t *cdb, uint8_t cdb_len, void *buf,
                   uint32_t len, int in)
{
    int r;
    if (!sc || !sc->tp || sc->sc_detached) {
        return -1;
    }
    if (sc->tp->enter && sc->tp->enter(sc)) {
        sc->sc_detached = 1;
        sc->sc_media.valid = 0;
        return -1;
    }
    r = command_locked(sc, cdb, cdb_len, buf, len, in);
    if (sc->tp->leave) {
        sc->tp->leave(sc);
    }
    return r;
}

static int request_sense(struct usb_softc *sc, uint8_t sense[18])
{
    const uint8_t cdb[6] = {3, 0, 0, 0, 18, 0};
    memset(sense, 0, 18);
    return command(sc, cdb, sizeof(cdb), sense, 18, 1);
}

int usb_storage_scsi_refresh_media(struct usb_softc *sc)
{
    const uint8_t tur[6] = {0}, capacity[10] = {0x25};
    uint8_t sense[18];
    struct scsi_read_capacity_10_data cap;
    unsigned attempt, attempts;
    if (!sc || !sc->tp) {
        return -1;
    }
    attempts = sc->tp->ready_attempts ? sc->tp->ready_attempts : 5;
    sc->sc_media.valid = 0;
    for (attempt = 0; attempt < attempts; ++attempt) {
        int r = command(sc, tur, sizeof(tur), NULL, 0, 1);
        if (!r) {
            break;
        }
        if (r < 0 || request_sense(sc, sense)) {
            return -1;
        }
        if ((sense[2] & 15) != 6 && (sense[2] & 15) != 2) {
            return -1;
        }
        delay(sc, 100);
    }
    if (attempt == attempts || command(sc, capacity, sizeof(capacity), &cap, sizeof(cap), 1)) {
        return -1;
    }
    sc->sc_media.last_lba = __builtin_bswap32(cap.returned_lba);
    sc->sc_media.block_size = __builtin_bswap32(cap.block_length);
    if (sc->sc_media.last_lba == UINT32_MAX ||
        (sc->sc_media.block_size != 512 && sc->sc_media.block_size != 1024 &&
         sc->sc_media.block_size != 2048 && sc->sc_media.block_size != 4096)) {
        return -1;
    }
    sc->sc_media.capacity_bytes = ((uint64_t)sc->sc_media.last_lba + 1) * sc->sc_media.block_size;
    sc->sc_media.valid = 1;
    sc->sc_media.write_protected = 0;
    return 0;
}

int usb_storage_scsi_blocks(struct usb_softc *sc, uint32_t lba, uint16_t count, void *buf,
                            size_t len, int write)
{
    uint8_t cdb[10] = {0};
    if (!sc || !sc->sc_media.valid || !count || !buf || lba > sc->sc_media.last_lba ||
        (uint32_t)count - 1 > sc->sc_media.last_lba - lba ||
        len != (uint32_t)count * sc->sc_media.block_size) {
        return -1;
    }
    cdb[0] = write ? 0x2a : 0x28;
    cdb[2] = lba >> 24;
    cdb[3] = lba >> 16;
    cdb[4] = lba >> 8;
    cdb[5] = lba;
    cdb[7] = count >> 8;
    cdb[8] = count;
    {
        int r = command(sc, cdb, sizeof(cdb), buf, len, !write);
        if (r == 1) {
            uint8_t sense[18];
            if (!request_sense(sc, sense)) {
                uint8_t key = (sense[0] & 0x7f) == 0x72 || (sense[0] & 0x7f) == 0x73
                                  ? sense[1] & 15
                                  : sense[2] & 15;
                if (key == 7) {
                    sc->sc_media.write_protected = 1;
                }
            }
        }
        return r ? -1 : 0;
    }
}

int usb_storage_scsi_read_sectors(struct usb_softc *sc, uint32_t lba, uint16_t count, void *buf,
                                  size_t len)
{
    size_t need = sc ? (size_t)count * sc->sc_media.block_size : 0;
    if (len < need) {
        return -1;
    }
    return usb_storage_scsi_blocks(sc, lba, count, buf, need, 0);
}

int usb_storage_scsi_sync(struct usb_softc *sc)
{
    const uint8_t cdb[10] = {0x35};
    return command(sc, cdb, sizeof(cdb), NULL, 0, 0) ? -1 : 0;
}
