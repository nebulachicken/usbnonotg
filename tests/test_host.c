/* Host-side tests for the forced-host enumeration/transport logic and for the
 * real SCSI layer, run against a fake Bulk-Only USB drive. The hardware parts
 * (otg_host.c) are replaced by the fakes below. */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "otg_host.h"
#include "usb_host.h"
#include "usb_storage.h"

/* ------------------------------------------------------------ fake device */
#define SECTORS 4096u
static uint8_t disk[SECTORS * 512];
static int op_depth, ops_total, session_alive = 1, fail_next_op;
static uint32_t fake_ms;
static unsigned fake_speed = 2, cur_addr, cur_mps;
static int configured, addressed, resets, halts_cleared;
static unsigned bulk_calls, max_chunk_seen;
static int not_ready_left; /* TEST UNIT READY answers NOT READY this many times */

/* device-side data toggle state per endpoint (0x81 IN, 0x02 OUT) */
static unsigned dev_toggle_in, dev_toggle_out;
static int toggle_errors;

enum { ST_IDLE, ST_DATA_OUT, ST_DATA_IN, ST_CSW };
static int state = ST_IDLE;
static uint8_t staged[16384];
static unsigned staged_len, staged_pos, out_expected, out_got;
static uint32_t cur_tag, cur_lba;
static uint8_t cur_status;
static uint8_t pending_sense;

static const uint8_t cfg_desc[] = {
    9, 2, 0, 0, 2, 1, 0, 0x80, 50, /* config: 2 interfaces (wTotalLength patched at use) */
    9, 4, 0, 0, 0, 3, 1, 1, 0,                            /* HID, no endpoints */
    9, 4, 1, 0, 2, 8, 6, 0x50, 0,                         /* mass storage BOT */
    7, 5, 0x81, 2, 0x00, 0x02, 0,                         /* bulk IN 512 */
    7, 5, 0x02, 2, 0x00, 0x02, 0,                         /* bulk OUT 512 */
};

void otg_log(const char *f, ...) { (void)f; }
size_t otg_log_dump(const char *p) { (void)p; return 0; }
const char *otg_strerror(int c) { (void)c; return ""; }
int otg_op_begin(void)
{
    if (fail_next_op || !session_alive) {
        fail_next_op = 0;
        return -1;
    }
    ++op_depth;
    ++ops_total;
    return 0;
}
void otg_op_end(void)
{
    assert(op_depth > 0);
    --op_depth;
}
uint32_t otg_now_ms(void) { return fake_ms; }
void otg_wait_ms(uint32_t ms) { fake_ms += ms; }
unsigned otg_host_speed(void) { return fake_speed; }
void otg_host_set_device(unsigned a, unsigned m)
{
    cur_addr = a;
    cur_mps = m;
}

int otg_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, const void *out,
                void *in, uint16_t length)
{
    (void)out;
    assert(op_depth > 0);
    if (type == 0x80 && req == 6 && (value >> 8) == 1) { /* device descriptor */
        uint8_t d[18] = {18, 1, 0, 2, 0, 0, 0, 64, 0x34, 0x12, 0x78, 0x56, 0, 1, 0, 0, 0, 1};
        unsigned n = length < 18 ? length : 18;
        assert(addressed ? cur_addr == 1 : cur_addr == 0);
        memcpy(in, d, n);
        return (int)n;
    }
    if (type == 0x00 && req == 5) {
        assert(cur_addr == 0 && value == 1);
        addressed = 1;
        return 0;
    }
    if (type == 0x80 && req == 6 && (value >> 8) == 2) { /* configuration */
        uint8_t c[sizeof cfg_desc];
        unsigned n = length < sizeof c ? length : (unsigned)sizeof c;
        assert(addressed && cur_addr == 1);
        memcpy(c, cfg_desc, sizeof c);
        c[2] = (uint8_t)sizeof c;
        memcpy(in, c, n);
        return (int)n;
    }
    if (type == 0x00 && req == 9) {
        assert(addressed && value == 1);
        configured = 1;
        return 0;
    }
    if (type == 0x21 && req == 0xff) { /* Bulk-Only reset */
        assert(index == 1);
        ++resets;
        dev_toggle_in = dev_toggle_out = 0;
        state = ST_IDLE;
        return 0;
    }
    if (type == 0x02 && req == 1) { /* CLEAR_FEATURE(ENDPOINT_HALT) */
        ++halts_cleared;
        return 0;
    }
    return -1;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void stage_in(const void *d, unsigned n)
{
    memcpy(staged, d, n);
    staged_len = n;
    staged_pos = 0;
}

static void handle_cbw(const uint8_t *c)
{
    uint32_t len = c[8] | c[9] << 8 | c[10] << 16 | (uint32_t)c[11] << 24;
    const uint8_t *cdb = c + 15;
    assert(c[0] == 'U' && c[1] == 'S' && c[2] == 'B' && c[3] == 'C');
    cur_tag = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
    cur_status = 0;
    switch (cdb[0]) {
    case 0x00: /* TEST UNIT READY */
        if (not_ready_left > 0) {
            --not_ready_left;
            cur_status = 1;
            pending_sense = 2;
        }
        state = ST_CSW;
        break;
    case 0x03: { /* REQUEST SENSE */
        uint8_t s[18] = {0x70, 0, pending_sense, 0, 0, 0, 0, 10};
        stage_in(s, 18);
        state = ST_DATA_IN;
        break;
    }
    case 0x25: { /* READ CAPACITY(10) */
        uint8_t d[8];
        put_be32(d, SECTORS - 1);
        put_be32(d + 4, 512);
        stage_in(d, 8);
        state = ST_DATA_IN;
        break;
    }
    case 0x28: /* READ(10) */
        cur_lba = (uint32_t)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
        assert(len == (unsigned)(cdb[7] << 8 | cdb[8]) * 512u && len <= sizeof staged);
        memcpy(staged, disk + cur_lba * 512u, len);
        staged_len = len;
        staged_pos = 0;
        state = ST_DATA_IN;
        break;
    case 0x2a: /* WRITE(10) */
        cur_lba = (uint32_t)cdb[2] << 24 | cdb[3] << 16 | cdb[4] << 8 | cdb[5];
        out_expected = len;
        out_got = 0;
        state = ST_DATA_OUT;
        break;
    case 0x35: /* SYNCHRONIZE CACHE */
        state = ST_CSW;
        break;
    default:
        assert(!"unexpected SCSI opcode");
    }
}

int otg_bulk_chunk(uint8_t ep, bool in, void *buf, unsigned len, unsigned mps, unsigned toggle,
                   uint32_t timeout_ms)
{
    unsigned *dev_toggle = in ? &dev_toggle_in : &dev_toggle_out;
    unsigned moved, packets;
    (void)timeout_ms;
    assert(op_depth > 0 && len > 0 && len <= OTG_BULK_CHUNK && mps == 512);
    assert(ep == (in ? 0x81 : 0x02));
    ++bulk_calls;
    if (len > max_chunk_seen) {
        max_chunk_seen = len;
    }
    if (toggle != *dev_toggle) {
        ++toggle_errors;
    }
    if (!in) {
        moved = len;
        packets = (len + mps - 1) / mps;
        if (state == ST_IDLE) {
            assert(len == 31);
            handle_cbw(buf);
        } else {
            assert(state == ST_DATA_OUT);
            memcpy(disk + cur_lba * 512u + out_got, buf, len);
            out_got += len;
            if (out_got >= out_expected) {
                state = ST_CSW;
            }
        }
    } else if (state == ST_DATA_IN) {
        unsigned left = staged_len - staged_pos;
        moved = len < left ? len : left;
        memcpy(buf, staged + staged_pos, moved);
        staged_pos += moved;
        packets = moved / mps + ((moved % mps || moved < len) ? 1 : 0);
        if (staged_pos >= staged_len) {
            state = ST_CSW;
        }
    } else {
        uint8_t csw[13] = {'U', 'S', 'B', 'S'};
        assert(state == ST_CSW && len == 13);
        put_le32(csw + 4, cur_tag);
        csw[12] = cur_status;
        memcpy(buf, csw, 13);
        moved = 13;
        packets = 1;
        state = ST_IDLE;
    }
    if (packets & 1u) {
        *dev_toggle ^= 1u;
    }
    return (int)moved;
}

/* ------------------------------------------------------------------ tests */
static void test_parse(void)
{
    struct usb_host_dev d;
    uint8_t c[sizeof cfg_desc];
    memcpy(c, cfg_desc, sizeof c);
    c[2] = (uint8_t)sizeof c;
    memset(&d, 0, sizeof d);
    assert(usb_host_parse_config(c, sizeof c, &d) == 1);
    assert(d.cfg == 1 && d.iface == 1 && d.ep_in == 0x81 && d.ep_out == 0x02);
    assert(d.mps_in == 512 && d.mps_out == 512);
    /* HID only: nothing to use */
    assert(usb_host_parse_config(c, 9 + 9, &d) == 0);
    /* truncated at every length: must never read past n or crash (ASan checks) */
    for (size_t n = 0; n < sizeof c; ++n) {
        uint8_t *copy = malloc(n ? n : 1);
        memcpy(copy, c, n);
        int r = usb_host_parse_config(copy, n, &d);
        assert(r == 0 || n == sizeof c);
        free(copy);
    }
    /* bogus zero-length descriptor must terminate the walk */
    c[9] = 0;
    assert(usb_host_parse_config(c, sizeof c, &d) == 0);
    /* wrong protocol (UAS 0x62) is not Bulk-Only */
    memcpy(c, cfg_desc, sizeof c);
    c[18 + 7] = 0x62;
    assert(usb_host_parse_config(c, sizeof c, &d) == 0);
    /* alternate setting 1 must be ignored */
    memcpy(c, cfg_desc, sizeof c);
    c[18 + 3] = 1;
    assert(usb_host_parse_config(c, sizeof c, &d) == 0);
}

static void test_packets(void)
{
    assert(usb_host_packets(1, 512, 512, 512) == 1);
    assert(usb_host_packets(1, 4096, 4096, 512) == 8);
    assert(usb_host_packets(1, 13, 13, 512) == 1);
    assert(usb_host_packets(1, 0, 13, 512) == 1);     /* zero-length packet */
    assert(usb_host_packets(1, 1024, 4096, 512) == 3); /* short: ended with a ZLP */
    assert(usb_host_packets(1, 100, 4096, 512) == 1);
    assert(usb_host_packets(0, 31, 31, 512) == 1);
    assert(usb_host_packets(0, 4096, 4096, 512) == 8);
    assert(usb_host_packets(0, 1, 1, 64) == 1);
    assert(usb_host_packets(0, 65, 65, 64) == 2);
}

static struct usb_softc sc;

static void test_attach_and_scsi(void)
{
    uint8_t buf[16384], expect[16384];
    unsigned i;
    for (i = 0; i < sizeof disk; ++i) {
        disk[i] = (uint8_t)(i * 7 + (i >> 9));
    }
    not_ready_left = 3;
    memset(&sc, 0, sizeof sc);
    assert(usb_host_attach(&sc) == 0);
    assert(configured && addressed && op_depth == 0);
    assert(sc.tp == &usb_host_transport && sc.host.ep_in == 0x81 && sc.sc_ifaceno == 1);

    assert(usb_storage_scsi_refresh_media(&sc) == 0);
    assert(op_depth == 0);
    assert(sc.sc_media.valid && sc.sc_media.block_size == 512);
    assert(sc.sc_media.last_lba == SECTORS - 1);
    assert(fake_ms >= 300); /* three NOT READY polls waited 100 ms each */

    /* single sector, then a 16 KiB read = four 4 KiB chunks */
    bulk_calls = 0;
    assert(usb_storage_scsi_blocks(&sc, 5, 1, buf, 512, 0) == 0);
    assert(memcmp(buf, disk + 5 * 512, 512) == 0);
    bulk_calls = 0;
    max_chunk_seen = 0;
    assert(usb_storage_scsi_blocks(&sc, 100, 32, buf, 16384, 0) == 0);
    assert(memcmp(buf, disk + 100 * 512, 16384) == 0);
    assert(bulk_calls == 1 + 4 + 1); /* CBW + 4 data chunks + CSW */
    assert(max_chunk_seen == OTG_BULK_CHUNK);

    /* write 16 KiB, read it back through the device */
    for (i = 0; i < sizeof expect; ++i) {
        expect[i] = (uint8_t)(0xA5 ^ i);
    }
    assert(usb_storage_scsi_blocks(&sc, 2000, 32, expect, 16384, 1) == 0);
    assert(memcmp(disk + 2000 * 512, expect, 16384) == 0);
    assert(usb_storage_scsi_blocks(&sc, 2000, 32, buf, 16384, 0) == 0);
    assert(memcmp(buf, expect, 16384) == 0);
    assert(usb_storage_scsi_sync(&sc) == 0);

    /* the host and the device never disagreed about a data toggle */
    assert(toggle_errors == 0);
    assert(op_depth == 0 && resets == 0);
}

static void test_session_loss(void)
{
    uint8_t buf[512];
    /* the session dies between commands: the drive must end up detached */
    session_alive = 0;
    assert(usb_storage_scsi_blocks(&sc, 0, 1, buf, 512, 0) == -1);
    assert(sc.sc_detached && !sc.sc_media.valid);
    session_alive = 1;
    assert(usb_storage_scsi_blocks(&sc, 0, 1, buf, 512, 0) == -1); /* stays dead */
    assert(op_depth == 0);
}

static void test_not_a_drive(void)
{
    /* a device that stays NOT READY forever: bounded wait, clean failure */
    struct usb_softc s2;
    uint32_t before = fake_ms;
    session_alive = 1;
    memset(&s2, 0, sizeof s2);
    state = ST_IDLE;
    addressed = configured = 0; /* a freshly reset device */
    dev_toggle_in = dev_toggle_out = 0;
    assert(usb_host_attach(&s2) == 0);
    not_ready_left = 100000;
    assert(usb_storage_scsi_refresh_media(&s2) == -1);
    assert(fake_ms - before >= 9000 && fake_ms - before <= 12000); /* ~10 s, not forever */
    assert(op_depth == 0);
}

int main(void)
{
    test_parse();
    test_packets();
    test_attach_and_scsi();
    test_session_loss();
    test_not_a_drive();
    puts("forced-host enumeration, chunked transport, toggles and SCSI layer tests passed");
    return 0;
}
