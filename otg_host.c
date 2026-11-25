/* Forced USB host mode for the TI-Nspire CX II top port (passive adapter).
 *
 * Everything in here is ported from otgweb.c: the register sequences (5 V on,
 * HNP arm variants A/B/C, role switch, port reset), the QH/qTD layout of the
 * polled control and bulk transfers, and the TI-OS handling (reference timer,
 * watchdog, USB register window, interrupt mask). What changed:
 *   - the session is persistent: after otg_host_open() the host stays up and
 *     every later burst of hardware access is bracketed by otg_op_begin/end();
 *   - the web/CDC parts are gone; bulk transfers are chunked (one 4 KiB qTD);
 *   - IRQ mode 1 (timed OS windows) is dropped. Mode 2 (TI-OS keeps running) is
 *     used, falling back to mode 0 (IRQs masked) as otgweb does.
 */
#include <os.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "otg_host.h"

#define R32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define R8(a) (*(volatile uint8_t *)(uintptr_t)(a))

/* FOTG210 controller of the top (mini-USB) port */
#define USBB 0xB0000000u
#define U(o) R32(USBB + (o))
enum {
    CAPLEN = 0x000,
    USBCMD = 0x010,
    USBSTS = 0x014,
    USBINTR = 0x018,
    ASYNCL = 0x028,
    PORTSC = 0x030,
    OTGCSR = 0x080,
    OTGISR = 0x084,
    OTGIER = 0x088,
    GIMR = 0x0C4,
    DEVCTRL = 0x100,
    PHYTMSR = 0x114,
    DMIGR2 = 0x13C,
    DIGR2 = 0x14C
};
#define CSR_B_BUS_REQ (1u << 0)
#define CSR_B_HNP_EN (1u << 1)
#define CSR_B_SESS_END (1u << 16)
#define CSR_B_SESS_VLD (1u << 17)
#define CSR_A_SESS_VLD (1u << 18)
#define CSR_A_VBUS_VLD (1u << 19)
#define CSR_ROLE_DEV (1u << 20)
#define CSR_ID_B (1u << 21)
#define CSR_SPEED(x) (((x) >> 22) & 3u)
#define CSR_VBUS_ANY (CSR_B_SESS_VLD | CSR_A_SESS_VLD | CSR_A_VBUS_VLD)
#define ISR_AVBUSERR (1u << 5)
#define ISR_OVD (1u << 10)
#define CMD_RUN (1u << 0)
#define CMD_PSE (1u << 4)
#define CMD_ASE (1u << 5)
#define CMD_IAAD (1u << 6)
#define STS_HALT (1u << 12)
#define STS_ASS (1u << 15)
#define PO_CONN (1u << 0)
#define PO_CSC (1u << 1)
#define PO_EN (1u << 2)
#define PO_PEC (1u << 3)
#define PO_RESET (1u << 8)

/* GPIO port 0 (byte registers, as TI-OS accesses them) */
#define GPIO0_DIR 0x90000010u /* 1 = input */
#define GPIO0_OUT 0x90000014u
#define VBUS_N (1u << 2) /* low = top port 5 V on */
#define CHG_OFF (1u << 6) /* high = charger disabled (TI-OS CHARGE_OFF) */

/* 32 kHz reference timer, programmed the same way nMedia does. */
#define TIMER_FAST 0x90010000u
#define TIMER_FIRST 0x900C0000u
#define TIMER_SECOND 0x900D0000u
#define TMR_LOAD (tmr_base + 0x00u)
#define TMR_VAL (tmr_base + 0x04u)
#define TMR_CTL (tmr_base + 0x08u)
#define TMR_SRC (tmr_base + 0x80u)
#define WD_LOAD 0x90060000u
#define WD_LOCK 0x90060C00u
#define VIC_BASE 0xDC000000u /* PL190 interrupt controller */

/* TI-OS gates the USB register window (PMU 0x90140050 bit 5) while the port is
 * idle; gated reads return zeros. */
#define PMU_GATE 0x90140050u
#define PMU_USBPWR 0x90140008u /* bits 10-11: top port controller/PHY power */
#define PMU_USBRDY 0x90140858u /* bits 2-3: top port PHY ready */
#define GATE_USB (1u << 5)

#define IRQ_MODE_DEFAULT 2 /* TI-OS keeps running while we poll */
#define CONNECT_WAIT_MS 2000u /* storage bridges can be slower than a dev board */

/* ---------------------------------------------------------------- log */
static char logbuf[8192];
static size_t loglen;
static bool log_on; /* chatty logging only while opening/closing a session */
static bool timer_on;
static uint32_t tmr_base = TIMER_FIRST;
static uint32_t t_saved_ctl, t_saved_src, t_saved_load, t_start, t_kick;

static inline uint32_t ticks(void) { return 0u - R32(TMR_VAL); }

static uint32_t now_ms(void)
{
    return timer_on ? (uint32_t)(((uint64_t)(ticks() - t_start) * 1000u) >> 15) : 0;
}

void otg_log(const char *format, ...)
{
    va_list ap;
    int n;
    if (loglen + 200 >= sizeof logbuf) {
        return;
    }
    loglen += (size_t)snprintf(logbuf + loglen, sizeof logbuf - loglen, "[%5lu] ",
                               (unsigned long)now_ms());
    va_start(ap, format);
    n = vsnprintf(logbuf + loglen, sizeof logbuf - loglen - 2, format, ap);
    va_end(ap);
    if (n > 0) {
        loglen += (size_t)n < sizeof logbuf - loglen - 2 ? (size_t)n : sizeof logbuf - loglen - 2;
    }
    logbuf[loglen++] = '\n';
    logbuf[loglen] = 0;
}

#define lg(...)                                                                                    \
    do {                                                                                           \
        if (log_on) {                                                                              \
            otg_log(__VA_ARGS__);                                                                  \
        }                                                                                          \
    } while (0)

size_t otg_log_dump(const char *program_path)
{
    char path[256];
    char *slash;
    FILE *f;
    size_t written = 0;
    snprintf(path, sizeof path, "%s", program_path ? program_path : "");
    slash = strrchr(path, '/');
    if (slash) {
        strcpy(slash + 1, "usb-storage_log.txt.tns");
    } else {
        /* Never fall back to the program's own path: that would overwrite it. */
        snprintf(path, sizeof path, "/documents/ndless/usb-storage_log.txt.tns");
    }
    f = fopen(path, "wb");
    if (f) {
        written = fwrite(logbuf, 1, loglen, f);
        fclose(f);
    }
    return written;
}

/* --------------------------------------------------- cpu / cache helpers */
#ifdef __arm__
static inline uint32_t cpsr_get(void)
{
    uint32_t c;
    __asm__ volatile("mrs %0, cpsr" : "=r"(c));
    return c;
}
static inline void cpsr_c_set(uint32_t c)
{
    __asm__ volatile("msr cpsr_c, %0" ::"r"(c) : "memory", "cc");
}
static void barrier(void)
{
    unsigned zero = 0;
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 4" ::"r"(zero) : "memory");
}
static void dma_clean(void *p, size_t size)
{
    uintptr_t end = ((uintptr_t)p + size + 31) & ~(uintptr_t)31;
    uintptr_t a;
    for (a = (uintptr_t)p & ~(uintptr_t)31; a < end; a += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c14, 1" ::"r"(a) : "memory");
    }
    barrier();
}
static void dma_invalidate(void *p, size_t size)
{
    uintptr_t end = ((uintptr_t)p + size + 31) & ~(uintptr_t)31;
    uintptr_t a;
    for (a = (uintptr_t)p & ~(uintptr_t)31; a < end; a += 32) {
        __asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
    }
}
#else /* only so the file can be syntax-checked on a PC */
static uint32_t fake_cpsr;
static inline uint32_t cpsr_get(void) { return fake_cpsr; }
static inline void cpsr_c_set(uint32_t c) { fake_cpsr = c; }
static void barrier(void) {}
static void dma_clean(void *p, size_t size) { (void)p; (void)size; }
static void dma_invalidate(void *p, size_t size) { (void)p; (void)size; }
#endif

/* ------------------------------------------------ timer / watchdog / irq */
static int irq_mode = IRQ_MODE_DEFAULT; /* may fall back to 0 */
static uint32_t os_cpsr; /* CPSR as TI-OS left it (IRQs on) */
static bool os_windows; /* TI-OS is allowed to run between our polls */

static void guard_os(void);

static void timer_begin(void)
{
    volatile unsigned i;
    t_saved_ctl = R32(TMR_CTL);
    t_saved_src = R32(TMR_SRC);
    t_saved_load = R32(TMR_LOAD);
    R32(TMR_CTL) = 0;
    R32(TMR_SRC) = 0x0A; /* 32768 Hz */
    R32(TMR_LOAD) = 0xFFFFFFFFu;
    R32(TMR_CTL) = 0x82; /* enabled, 32-bit, free running */
    for (i = 0; i < 20000; ++i) { /* let the reload land before taking the baseline */
    }
    t_start = t_kick = ticks();
    timer_on = true;
}

static void timer_end(void)
{
    timer_on = false;
    R32(TMR_CTL) = 0;
    R32(TMR_SRC) = t_saved_src;
    R32(TMR_LOAD) = t_saved_load;
    R32(TMR_CTL) = t_saved_ctl;
}

/* TI-OS reloads the SP805 watchdog with the same value from a task. We do it too
 * (unlock, reload, lock); with IRQs possibly on it must not interleave with
 * TI-OS's own kick, so it runs with IRQs off. */
static void wd_kick(void)
{
    uint32_t c = cpsr_get();
    uint32_t load;
    cpsr_c_set(c | 0xC0);
    load = R32(WD_LOAD);
    if (load) {
        R32(WD_LOCK) = 0x1ACCE551u;
        R32(WD_LOAD) = load;
        R32(WD_LOCK) = 0;
    }
    cpsr_c_set(c);
}

static void service(void)
{
    uint32_t t = ticks();
    if (t - t_kick > 3277u) { /* ~100 ms */
        wd_kick();
        t_kick = ticks();
    }
    if (os_windows) {
        guard_os();
    }
}

static void wait_ms(uint32_t ms)
{
    uint32_t s = ticks(), n = (uint32_t)(((uint64_t)ms << 15) / 1000u) + 1;
    while (ticks() - s < n) {
        service();
    }
}

static void os_windows_on(void)
{
    if (irq_mode == 0) {
        return;
    }
    os_windows = true;
    cpsr_c_set(os_cpsr);
}

static void os_windows_off(void)
{
    if (!os_windows) {
        return;
    }
    os_windows = false;
    cpsr_c_set(os_cpsr | 0xC0);
}

static uint32_t usb_ungate(void)
{
    uint32_t saved = R32(PMU_GATE);
    unsigned polls;
    if (saved & GATE_USB) {
        R32(PMU_GATE) = saved & ~GATE_USB;
        barrier();
    }
    for (polls = 0; polls < 200000u && (U(CAPLEN) & 0xffff00ffu) != 0x01000010u; ++polls) {
    }
    return saved;
}

static void usb_regate(uint32_t saved)
{
    if (saved & GATE_USB) {
        R32(PMU_GATE) = R32(PMU_GATE) | GATE_USB;
        barrier();
    }
}

static bool readable(uint32_t cap) { return (cap & 0xffff00ffu) == 0x01000010u; }

/* TI-OS runs now, so undo anything it does to the USB block. */
static void guard_os(void)
{
    uint32_t c = cpsr_get();
    cpsr_c_set(c | 0xC0);
    if (R32(PMU_GATE) & GATE_USB) {
        R32(PMU_GATE) = R32(PMU_GATE) & ~GATE_USB;
        barrier();
    }
    if ((U(GIMR) & 7u) != 7u) {
        U(GIMR) = U(GIMR) | 7u;
    }
    cpsr_c_set(c);
}

/* Which timer is TI-OS ticking from? Ask the interrupt controller which IRQs are
 * enabled (17 fast timer, 18 first, 19 second) and use a timer whose IRQ is not
 * enabled as the free-running reference. Mode 0 keeps the first timer. Returns
 * false when no timer is free. */
static bool pick_timer(void)
{
    static const struct {
        uint32_t base;
        unsigned irq;
        const char *name;
    } cand[] = {{TIMER_FIRST, 18, "first"}, {TIMER_SECOND, 19, "second"}, {TIMER_FAST, 17, "fast"}};
    uint32_t en = R32(VIC_BASE + 0x10u);
    unsigned i;
    tmr_base = TIMER_FIRST;
    if (irq_mode == 0) {
        return true;
    }
    for (i = 0; i < 3; ++i) {
        if (!((en >> cand[i].irq) & 1u)) {
            tmr_base = cand[i].base;
            lg("reference timer: %s (vic enabled=%08lx)", cand[i].name, (unsigned long)en);
            return true;
        }
    }
    lg("every timer IRQ is enabled: falling back to IRQ mode 0");
    return false;
}

/* Interrupts off, reference timer running. TI-OS is not let in yet. */
static int op_enter_raw(void)
{
    uint32_t saved = cpsr_get();
    uint32_t t0;
    unsigned n = 0;
    os_cpsr = saved;
    cpsr_c_set(saved | 0xC0);
    if ((cpsr_get() & 0xC0) != 0xC0) {
        cpsr_c_set(saved);
        return OTG_E_ENV;
    }
    /* IRQs already masked on entry: nobody would ever unmask them for us. */
    irq_mode = (saved & 0xC0) ? 0 : IRQ_MODE_DEFAULT;
    if (!pick_timer()) {
        irq_mode = 0;
    }
    timer_begin();
    t0 = ticks();
    while (ticks() == t0 && n < 3000000u) {
        ++n;
    }
    if (ticks() == t0) {
        timer_end();
        cpsr_c_set(saved);
        return OTG_E_ENV;
    }
    return 0;
}

static void op_leave_raw(void)
{
    os_windows_off();
    if (timer_on) {
        timer_end();
    }
    cpsr_c_set(os_cpsr);
}

/* ------------------------------------------------------------ dma memory */
typedef struct {
    uint32_t next, alt, token, buffer[5];
} Qtd;
typedef struct {
    uint32_t next, info1, info2, current;
    Qtd overlay;
    uint32_t pad[4];
} Qh;

/* Static, so it stays valid after the launcher exits (the program is resident). */
static uint8_t dma_pool[3 * 4096 + 4096];
static Qh *qh;
static Qtd *qtd;
static uint8_t *setup_buf, *data_buf; /* 4 KiB each, 4 KiB aligned */

static void dma_setup(void)
{
    uint8_t *base = (uint8_t *)(((uintptr_t)dma_pool + 4095) & ~(uintptr_t)4095);
    memset(base, 0, 3 * 4096);
    qh = (Qh *)base; /* 64 bytes, 32-byte aligned */
    qtd = (Qtd *)(base + 256); /* 3 x 32 bytes */
    setup_buf = base + 4096;
    data_buf = base + 8192;
    dma_clean(base, 3 * 4096);
}

static void qtd_init(Qtd *q, void *buffer, unsigned length, unsigned pid, unsigned toggle)
{
    uintptr_t a = (uintptr_t)buffer;
    unsigned i;
    memset(q, 0, sizeof *q);
    q->next = q->alt = 1;
    q->token = (length << 16) | (toggle << 31) | (3u << 10) | (pid << 8) | 0x80;
    q->buffer[0] = (uint32_t)a;
    for (i = 1; i < 5; ++i) {
        q->buffer[i] = (uint32_t)((a + 4096 * i) & ~(uintptr_t)4095);
    }
}

static bool wait_reg(unsigned off, uint32_t mask, uint32_t value, uint32_t ms)
{
    uint32_t s = now_ms();
    while ((U(off) & mask) != value) {
        service();
        if (now_ms() - s > ms) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------ session */
static struct {
    bool open;
    bool powered; /* we powered the controller up */
    bool state_saved;
    bool vbus_on;
    bool host_started;
    uint32_t gate, pm8;
    uint32_t s_cmd, s_intr, s_async, s_ier, s_gimr, s_phy, s_dmigr2;
    uint8_t s_dir, s_out;
    unsigned speed; /* 0 FS, 1 LS, 2 HS (OTGCSR bits 22-23 in host role) */
} sess;

static unsigned ep0_mps = 8, dev_addr;
static unsigned op_depth;

static void snap(const char *tag)
{
    lg("%s: otgcsr=%08lx otgisr=%08lx cmd=%08lx sts=%08lx port=%08lx phy=%08lx digr2=%08lx gpio0 "
       "dir=%02x out=%02x",
       tag, (unsigned long)U(OTGCSR), (unsigned long)U(OTGISR), (unsigned long)U(USBCMD),
       (unsigned long)U(USBSTS), (unsigned long)U(PORTSC), (unsigned long)U(PHYTMSR),
       (unsigned long)U(DIGR2), R8(GPIO0_DIR), R8(GPIO0_OUT));
}

static void describe_csr(char *out, size_t n, uint32_t c)
{
    snprintf(out, n, "ID=%c role=%s vbus[%s%s%s%s] speed=%lu", (c & CSR_ID_B) ? 'B' : 'A',
             (c & CSR_ROLE_DEV) ? "device" : "HOST", (c & CSR_A_VBUS_VLD) ? " AVBUS" : "",
             (c & CSR_A_SESS_VLD) ? " ASESS" : "", (c & CSR_B_SESS_VLD) ? " BSESS" : "",
             (c & CSR_B_SESS_END) ? " BEND" : "", (unsigned long)CSR_SPEED(c));
}

/* Poll OTGCSR/OTGISR/port for `ms`, logging every change. Stops early when
 * `until` returns true. Returns false on over-current / VBUS error. */
static bool watch(uint32_t ms, bool (*until)(void), const char *what)
{
    uint32_t s = now_ms(), lc = U(OTGCSR), li = U(OTGISR), lp = U(PORTSC), ld = U(DIGR2);
    char d[96];
    describe_csr(d, sizeof d, lc);
    lg("%s: watching %lu ms; %s", what, (unsigned long)ms, d);
    while (now_ms() - s < ms) {
        uint32_t c, i, p, g;
        service();
        c = U(OTGCSR);
        i = U(OTGISR);
        p = U(PORTSC);
        g = U(DIGR2);
        if (c != lc || i != li || p != lp || g != ld) {
            describe_csr(d, sizeof d, c);
            lg("  csr=%08lx (%s) isr=%08lx port=%08lx digr2=%08lx", (unsigned long)c, d,
               (unsigned long)i, (unsigned long)p, (unsigned long)g);
            lc = c;
            li = i;
            lp = p;
            ld = g;
        }
        if (i & (ISR_OVD | ISR_AVBUSERR)) {
            otg_log("  !! over-current / VBUS error reported (isr=%08lx), stopping",
                    (unsigned long)i);
            return false;
        }
        if (until && until()) {
            return true;
        }
    }
    return true;
}

static bool vbus_valid(void) { return (U(OTGCSR) & (CSR_B_SESS_VLD | CSR_A_VBUS_VLD)) != 0; }
static bool role_host(void) { return !(U(OTGCSR) & CSR_ROLE_DEV); }
static bool port_connected(void) { return (U(PORTSC) & PO_CONN) != 0; }

/* Undo everything open_session() did, in the order otgweb's restore path uses. */
static void session_teardown(void)
{
    bool saved_log = log_on;
    log_on = true;
    (void)usb_ungate();
    lg("restore");
    if (sess.host_started) {
        U(USBCMD) &= ~(CMD_ASE | CMD_PSE);
        wait_reg(USBSTS, STS_ASS, 0, 30);
        U(USBCMD) &= ~CMD_RUN;
        wait_reg(USBSTS, STS_HALT, STS_HALT, 30);
    }
    if (sess.state_saved) {
        U(ASYNCL) = sess.s_async;
        U(OTGCSR) = U(OTGCSR) & ~(CSR_B_BUS_REQ | CSR_B_HNP_EN);
    }
    if (sess.vbus_on) {
        /* 5 V off like TI_JUNGO_USB_os_turn_vbus_off(0), then GPIO2 back to how
         * TI-OS left it (never let the output latch drive it low again) */
        R8(GPIO0_OUT) = (uint8_t)(R8(GPIO0_OUT) | VBUS_N);
        if (sess.s_dir & VBUS_N) {
            R8(GPIO0_DIR) = (uint8_t)(R8(GPIO0_DIR) | VBUS_N);
            R8(GPIO0_OUT) = (uint8_t)((R8(GPIO0_OUT) & ~VBUS_N) | (sess.s_out & VBUS_N));
        }
        watch(800, NULL, "vbus off");
    }
    if (sess.state_saved) {
        U(PHYTMSR) = sess.s_phy;
        U(DMIGR2) = sess.s_dmigr2;
        U(USBCMD) = sess.s_cmd;
        U(USBINTR) = sess.s_intr;
        U(OTGIER) = sess.s_ier;
        U(GIMR) = sess.s_gimr;
        snap("end");
    }
    if (sess.powered) {
        R32(PMU_USBPWR) = R32(PMU_USBPWR) & ~0xC00u;
        R32(PMU_USBPWR) = R32(PMU_USBPWR) | (1u << 24);
        barrier();
        lg("powered down again: pmu08=%08lx (was %08lx)", (unsigned long)R32(PMU_USBPWR),
           (unsigned long)sess.pm8);
    }
    usb_regate(sess.gate);
    sess.open = false;
    sess.powered = sess.state_saved = sess.vbus_on = sess.host_started = false;
    log_on = saved_log;
}

/* Ports otgweb's main() start-up checks and run_test() steps 1-3. Runs between
 * op_enter_raw() and op_leave_raw(). On failure the hardware is restored. */
static int open_session(void)
{
    static const char *const variants[] = {"A: pull-up on", "B: pull-up off", "C: pull-up pulse"};
    uint32_t cap, csr, t;
    unsigned v;
    bool host = false;
    int rc;

    sess.gate = usb_ungate();
    sess.pm8 = R32(PMU_USBPWR);
    cap = U(CAPLEN);
    lg("open: irq_mode=%d gate=%08lx pmu08=%08lx pmu858=%08lx cap=%08lx dir=%02x out=%02x wd=%08lx",
       irq_mode, (unsigned long)sess.gate, (unsigned long)sess.pm8,
       (unsigned long)R32(PMU_USBRDY), (unsigned long)cap, R8(GPIO0_DIR), R8(GPIO0_OUT),
       (unsigned long)R32(WD_LOAD));
    if (!readable(cap)) {
        uint32_t s0;
        lg("controller powered down: PMU 0x08 |= 0x01000C00");
        R32(PMU_USBPWR) = sess.pm8 | 0x01000C00u;
        barrier();
        sess.powered = true;
        s0 = now_ms();
        while (now_ms() - s0 < 500 &&
               ((R32(PMU_USBRDY) & 0xCu) != 0xCu || !readable(U(CAPLEN)))) {
            service();
        }
        wait_ms(20);
        cap = U(CAPLEN);
        lg("after power-up (%lu ms): pmu08=%08lx pmu858=%08lx cap=%08lx devctrl=%08lx",
           (unsigned long)(now_ms() - s0), (unsigned long)R32(PMU_USBPWR),
           (unsigned long)R32(PMU_USBRDY), (unsigned long)cap, (unsigned long)U(DEVCTRL));
    }
    csr = U(OTGCSR);
    if (!readable(cap)) {
        rc = OTG_E_POWER;
        goto fail;
    }
    if (!(csr & CSR_ID_B)) {
        lg("ID pin reads grounded (csr=%08lx): OTG adapter", (unsigned long)csr);
        rc = OTG_E_OTG_ADAPTER;
        goto fail;
    }
    if (csr & CSR_VBUS_ANY) {
        lg("USB power already present (csr=%08lx)", (unsigned long)csr);
        rc = OTG_E_POWER_PRESENT;
        goto fail;
    }
    if (!(R8(GPIO0_OUT) & CHG_OFF)) {
        lg("charger not switched off by TI-OS");
        rc = OTG_E_CHARGER;
        goto fail;
    }
    if (sess.powered && !(U(DEVCTRL) & 0x20u)) {
        U(DEVCTRL) = U(DEVCTRL) | 0x20u;
        lg("device controller chip-enable set");
    }

    sess.s_cmd = U(USBCMD);
    sess.s_intr = U(USBINTR);
    sess.s_async = U(ASYNCL);
    sess.s_ier = U(OTGIER);
    sess.s_gimr = U(GIMR);
    sess.s_phy = U(PHYTMSR);
    sess.s_dmigr2 = U(DMIGR2);
    sess.s_dir = R8(GPIO0_DIR);
    sess.s_out = R8(GPIO0_OUT);
    sess.state_saved = true;

    snap("start");
    U(GIMR) = sess.s_gimr | 7u; /* we poll; TI-OS sees nothing until restore */
    os_windows_on(); /* IRQ mode 2: TI-OS may run from here on */

    /* 1. 5 V on, exactly like TI_JUNGO_USB_os_turn_vbus_on(0) */
    lg("step 1: GPIO2 -> output low (top port 5 V on)");
    R8(GPIO0_DIR) = (uint8_t)(R8(GPIO0_DIR) & ~VBUS_N);
    R8(GPIO0_OUT) = (uint8_t)(R8(GPIO0_OUT) & ~VBUS_N);
    sess.vbus_on = true;
    t = now_ms();
    if (!watch(400, vbus_valid, "vbus")) {
        rc = OTG_E_FAULT;
        goto fail;
    }
    if (!vbus_valid()) {
        lg("VBUS not seen by the controller within 400 ms");
        rc = OTG_E_NO_VBUS;
        goto fail;
    }
    lg("VBUS valid after %lu ms", (unsigned long)(now_ms() - t));
    if (!watch(60, NULL, "vbus settle")) {
        rc = OTG_E_FAULT;
        goto fail;
    }

    /* 2. Arm HNP like TI-OS's f210_otg driver does (cmd 7): mask suspend/resume
     *    device interrupts, then B_BUS_REQ and B_HNP_EN. */
    t = now_ms();
    for (v = 0; v < 3 && !host; ++v) {
        lg("step 2%c: arm HNP, variant %s", (char)('a' + v), variants[v]);
        U(OTGCSR) = U(OTGCSR) & ~(CSR_B_BUS_REQ | CSR_B_HNP_EN);
        if (v == 0) {
            U(PHYTMSR) = U(PHYTMSR) & ~1u;
        }
        if (v == 1) {
            U(PHYTMSR) = U(PHYTMSR) | 1u;
        }
        if (v == 2) {
            U(PHYTMSR) = U(PHYTMSR) & ~1u;
            wait_ms(30);
        }
        U(DMIGR2) = U(DMIGR2) | 2u | 4u;
        U(OTGCSR) = U(OTGCSR) | CSR_B_BUS_REQ;
        U(OTGCSR) = U(OTGCSR) | CSR_B_HNP_EN;
        if (v == 2) {
            wait_ms(5);
            U(PHYTMSR) = U(PHYTMSR) | 1u;
        }
        if (!watch(v == 0 ? 1500 : 900, role_host, "hnp")) {
            rc = OTG_E_FAULT;
            goto fail;
        }
        host = role_host();
    }
    if (!host) {
        lg("controller stayed in device role");
        rc = OTG_E_NO_HOST;
        goto fail;
    }
    lg("ROLE SWITCHED TO HOST after %lu ms (variant %s)", (unsigned long)(now_ms() - t),
       variants[v - 1]);
    snap("host");

    /* 3. Host side: run, wait for connect, reset */
    U(USBCMD) = (U(USBCMD) & ~(CMD_ASE | CMD_PSE)) | CMD_RUN;
    sess.host_started = true;
    if (!wait_reg(USBSTS, STS_HALT, 0, 30)) {
        lg("host controller did not leave halt (sts=%08lx)", (unsigned long)U(USBSTS));
    }
    if (!watch(CONNECT_WAIT_MS, port_connected, "connect")) {
        rc = OTG_E_FAULT;
        goto fail;
    }
    if (!port_connected()) {
        lg("no device connection on the port");
        rc = OTG_E_NO_DEVICE;
        goto fail;
    }
    lg("port reset");
    U(PORTSC) = (U(PORTSC) & ~(PO_CSC | PO_PEC | PO_EN)) | PO_RESET;
    wait_ms(55);
    U(PORTSC) = U(PORTSC) & ~(PO_CSC | PO_PEC | PO_RESET);
    wait_reg(PORTSC, PO_RESET, 0, 50);
    wait_ms(20);
    snap("after reset");
    if (!(U(PORTSC) & PO_EN)) {
        lg("port not enabled after reset");
        rc = OTG_E_NOT_ENABLED;
        goto fail;
    }
    sess.speed = CSR_SPEED(U(OTGCSR));
    os_windows_off();
    return OTG_OK;

fail:
    os_windows_off();
    session_teardown();
    return rc;
}

int otg_host_probe(void)
{
    uint32_t c = cpsr_get();
    uint32_t saved, cap, csr;
    cpsr_c_set(c | 0xC0);
    saved = usb_ungate();
    cap = U(CAPLEN);
    csr = U(OTGCSR);
    usb_regate(saved);
    cpsr_c_set(c);
    /* A powered-down controller cannot be read: the checks are repeated by
     * otg_host_open() once it has been powered up. */
    if (readable(cap) && !(csr & CSR_ID_B)) {
        return OTG_PROBE_OTG_ADAPTER;
    }
    if (readable(cap) && (csr & CSR_VBUS_ANY)) {
        return OTG_PROBE_POWER_PRESENT;
    }
    if (!(R8(GPIO0_OUT) & CHG_OFF)) {
        return OTG_PROBE_CHARGER_ON;
    }
    return OTG_PROBE_PASSIVE;
}

int otg_host_open(void)
{
    int rc;
    if (sess.open) {
        return OTG_E_BUSY;
    }
    memset(&sess, 0, sizeof sess);
    op_depth = 0;
    ep0_mps = 8;
    dev_addr = 0;
    log_on = true;
    otg_log("otg_host_open: forced host mode");
    rc = op_enter_raw();
    if (rc) {
        otg_log("cannot take over interrupts/timer (%d)", rc);
        log_on = false;
        return rc;
    }
    dma_setup();
    rc = open_session();
    if (rc == OTG_OK) {
        sess.open = true;
        otg_log("session open, device speed %u", sess.speed);
    } else {
        otg_log("open failed: %s", otg_strerror(rc));
    }
    op_leave_raw();
    log_on = false;
    return rc;
}

void otg_host_close(void)
{
    if (!sess.open) {
        return;
    }
    if (op_depth == 0) {
        if (op_enter_raw()) {
            return; /* cannot restore safely right now; try again later */
        }
        session_teardown();
        op_leave_raw();
    } else {
        session_teardown();
    }
}

bool otg_host_active(void) { return sess.open; }

/* Cheap health check of a live session, with IRQs off and the window ungated. */
static bool session_alive(const char **why)
{
    uint32_t p;
    if (!readable(U(CAPLEN))) {
        *why = "controller not readable";
        return false;
    }
    if (U(OTGCSR) & CSR_ROLE_DEV) {
        *why = "controller left host role";
        return false;
    }
    if (U(OTGISR) & (ISR_OVD | ISR_AVBUSERR)) {
        *why = "over-current / VBUS error";
        return false;
    }
    p = U(PORTSC);
    if (!(p & PO_CONN)) {
        *why = "device disconnected";
        return false;
    }
    if (!(p & PO_EN)) {
        *why = "port no longer enabled";
        return false;
    }
    if (U(USBSTS) & STS_HALT) {
        *why = "host controller halted";
        return false;
    }
    return true;
}

/* Optional task callback: keep the USB register window available while
 * Documents is idle. The same storage lock used by filesystem callbacks
 * must be held by the caller so this cannot overlap a SCSI transaction.
 * This only checks/guards the existing session; it does not enumerate or
 * attempt to re-power a device that has already disconnected.
 */
int otg_host_background_tick(void)
{
    const char *why = "unknown";
    if (!sess.open) {
        return -1;
    }
    if (op_depth || op_enter_raw()) {
        return 1; /* let the worker retry after the current OS activity */
    }
    (void)usb_ungate();
    if (!session_alive(&why)) {
        otg_log("background tick: session lost: %s", why);
        session_teardown();
        op_leave_raw();
        return -1;
    }
    guard_os();
    op_leave_raw();
    return 0;
}

int otg_op_begin(void)
{
    const char *why = "";
    if (op_depth) {
        ++op_depth;
        return 0;
    }
    if (!sess.open) {
        return -1;
    }
    if (op_enter_raw()) {
        otg_log("op_begin: cannot take over interrupts/timer");
        return -1;
    }
    (void)usb_ungate(); /* TI-OS may have re-gated the window while we were idle */
    if (!session_alive(&why)) {
        otg_log("session lost: %s", why);
        session_teardown();
        op_leave_raw();
        return -1;
    }
    U(GIMR) = U(GIMR) | 7u;
    os_windows_on();
    op_depth = 1;
    return 0;
}

void otg_op_end(void)
{
    if (!op_depth || --op_depth) {
        return;
    }
    op_leave_raw();
}

uint32_t otg_now_ms(void) { return now_ms(); }
void otg_wait_ms(uint32_t ms)
{
    if (op_depth) {
        wait_ms(ms);
    }
}
unsigned otg_host_speed(void) { return sess.speed; }
void otg_host_set_device(unsigned addr, unsigned mps)
{
    dev_addr = addr;
    ep0_mps = mps;
}

/* ------------------------------------------------------------ transfers */

/* One control transfer (same QH/qTD layout as nMedia's hardware-tested
 * usb_audio.c). IN data is left in data_buf. Returns bytes moved or -1. */
static int control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, const void *out,
                   uint16_t length)
{
    bool input = (type & 0x80) != 0;
    uint32_t s;
    int result = -1;
    if (length > 4096) {
        length = 4096;
    }
    setup_buf[0] = type;
    setup_buf[1] = req;
    setup_buf[2] = value & 0xFF;
    setup_buf[3] = value >> 8;
    setup_buf[4] = index & 0xFF;
    setup_buf[5] = index >> 8;
    setup_buf[6] = length & 0xFF;
    setup_buf[7] = length >> 8;
    memset(data_buf, 0, 4096);
    if (!input && length && out) {
        memcpy(data_buf, out, length);
    }
    memset(qh, 0, sizeof *qh);
    qtd_init(qtd, setup_buf, 8, 2, 0);
    qtd_init(qtd + 1, data_buf, length, input ? 1 : 0, 1);
    qtd_init(qtd + 2, data_buf, 0, input ? 0 : 1, 1);
    qtd[0].next = (uint32_t)(uintptr_t)(qtd + (length ? 1 : 2));
    qtd[1].next = qtd[1].alt = (uint32_t)(uintptr_t)(qtd + 2);
    qh->next = (uint32_t)(uintptr_t)qh | 2;
    qh->info1 = dev_addr | ((uint32_t)ep0_mps << 16) | (1u << 14) | (1u << 15) |
                (sess.speed == 2 ? 2u << 12 : (sess.speed == 1 ? 1u << 12 : 0u) | (1u << 27)) |
                (4u << 28);
    qh->info2 = 1u << 30;
    qh->overlay.next = (uint32_t)(uintptr_t)qtd;
    qh->overlay.alt = 1;
    dma_clean(setup_buf, 4096 * 2);
    dma_clean(qtd, 3 * sizeof(Qtd));
    dma_clean(qh, sizeof *qh);
    U(ASYNCL) = (uint32_t)(uintptr_t)qh;
    U(USBCMD) = (U(USBCMD) & ~(CMD_ASE | CMD_IAAD)) | CMD_RUN | CMD_ASE;
    if (!wait_reg(USBSTS, STS_ASS, STS_ASS, 50)) {
        otg_log("  control %02x/%02x: async schedule did not start (sts=%08lx)", type, req,
                (unsigned long)U(USBSTS));
        return -1;
    }
    s = now_ms();
    for (;;) {
        uint32_t t2;
        service();
        dma_invalidate(qtd, 3 * sizeof(Qtd));
        t2 = qtd[2].token;
        if (!(t2 & 0x80)) {
            if (!(t2 & 0x7c)) {
                result = length ? (int)length - (int)((qtd[1].token >> 16) & 0x7fff) : 0;
            }
            break;
        }
        if ((qtd[0].token | qtd[1].token) & 0x7c) {
            break;
        }
        if (now_ms() - s > 500) {
            break;
        }
    }
    if (result < 0) {
        otg_log("  control %02x/%02x val=%04x idx=%04x len=%u failed: q0=%08lx q1=%08lx q2=%08lx "
                "port=%08lx sts=%08lx",
                type, req, value, index, length, (unsigned long)qtd[0].token,
                (unsigned long)qtd[1].token, (unsigned long)qtd[2].token,
                (unsigned long)U(PORTSC), (unsigned long)U(USBSTS));
    }
    U(USBCMD) &= ~CMD_ASE;
    wait_reg(USBSTS, STS_ASS, 0, 50);
    dma_invalidate(data_buf, 4096);
    return result;
}

int otg_control(uint8_t type, uint8_t req, uint16_t value, uint16_t index, const void *out,
                void *in, uint16_t length)
{
    int r;
    if (!sess.open || !op_depth) {
        return -1;
    }
    r = control(type, req, value, index, out, length);
    if (r > 0 && in && (type & 0x80)) {
        memcpy(in, data_buf, (size_t)r);
    }
    return r;
}

/* One bulk chunk on the async schedule: a single qTD, so a single page. Unlike
 * otgweb's per-packet slices this waits for the whole chunk (a device that NAKs
 * all along simply runs into timeout_ms); a timed-out chunk makes the caller
 * abort the whole SCSI command and do BOT reset recovery, which also resets the
 * data toggles, so no partial progress has to be tracked. */
int otg_bulk_chunk(uint8_t ep, bool in, void *buf, unsigned len, unsigned mps, unsigned toggle,
                   uint32_t timeout_ms)
{
    uint32_t s, tok;
    bool fault = false;
    unsigned remaining, moved;
    if (!sess.open || !op_depth || !buf || !len || len > OTG_BULK_CHUNK || !mps) {
        return OTG_XFER_ERROR;
    }
    if (in) {
        memset(data_buf, 0, len);
    } else {
        memcpy(data_buf, buf, len);
    }
    memset(qh, 0, sizeof *qh);
    qtd_init(qtd, data_buf, len, in ? 1 : 0, toggle);
    qtd[0].next = qtd[0].alt = 1;
    qh->next = (uint32_t)(uintptr_t)qh | 2;
    qh->info1 = dev_addr | ((uint32_t)(ep & 15) << 8) | ((uint32_t)mps << 16) | (1u << 14) |
                (1u << 15) | (sess.speed == 2 ? 2u << 12 : (sess.speed == 1 ? 1u << 12 : 0u)) |
                (4u << 28);
    qh->info2 = 1u << 30;
    qh->overlay.next = (uint32_t)(uintptr_t)qtd;
    qh->overlay.alt = 1;
    dma_clean(data_buf, 4096);
    dma_clean(qtd, sizeof(Qtd));
    dma_clean(qh, sizeof *qh);
    U(ASYNCL) = (uint32_t)(uintptr_t)qh;
    U(USBCMD) = (U(USBCMD) & ~(CMD_ASE | CMD_IAAD)) | CMD_RUN | CMD_ASE;
    if (!wait_reg(USBSTS, STS_ASS, STS_ASS, 50)) {
        otg_log("  bulk %s: async schedule did not start (sts=%08lx)", in ? "in" : "out",
                (unsigned long)U(USBSTS));
        return OTG_XFER_ERROR;
    }
    s = now_ms();
    for (;;) {
        service();
        dma_invalidate(qtd, sizeof(Qtd));
        if (!(qtd[0].token & 0x80)) {
            break;
        }
        if (U(OTGISR) & (ISR_OVD | ISR_AVBUSERR)) {
            fault = true;
            break;
        }
        if (!(U(PORTSC) & PO_CONN)) {
            break;
        }
        if (now_ms() - s > timeout_ms) {
            break;
        }
    }
    U(USBCMD) &= ~CMD_ASE;
    wait_reg(USBSTS, STS_ASS, 0, 50);
    dma_invalidate(qtd, sizeof(Qtd)); /* it may have completed while the schedule stopped */
    tok = qtd[0].token;
    if (fault) {
        otg_log("  bulk %s: over-current / VBUS error", in ? "in" : "out");
        return OTG_XFER_ERROR;
    }
    if (tok & 0x80) {
        otg_log("  bulk %s ep %02x: no completion, token=%08lx port=%08lx", in ? "in" : "out", ep,
                (unsigned long)tok, (unsigned long)U(PORTSC));
        return OTG_XFER_TIMEOUT;
    }
    if (tok & 0x40) {
        otg_log("  bulk %s: halted, token=%08lx", in ? "in" : "out", (unsigned long)tok);
        return (tok & 0x3c) ? OTG_XFER_ERROR : OTG_XFER_STALL;
    }
    if (tok & 0x3c) {
        otg_log("  bulk %s: error token=%08lx", in ? "in" : "out", (unsigned long)tok);
        return OTG_XFER_ERROR;
    }
    remaining = (tok >> 16) & 0x7fff;
    moved = remaining < len ? len - remaining : 0;
    if (in) {
        dma_invalidate(data_buf, 4096);
        memcpy(buf, data_buf, moved);
    }
    return (int)moved;
}

const char *otg_strerror(int code)
{
    switch (code) {
    case OTG_OK:
        return "ok";
    case OTG_E_BUSY:
        return "a forced-host session is already open";
    case OTG_E_ENV:
        return "cannot take over the CPU interrupts/timer";
    case OTG_E_POWER:
        return "the USB controller did not power up";
    case OTG_E_OTG_ADAPTER:
        return "the ID pin is grounded: this is an OTG adapter, TI-OS is already the host";
    case OTG_E_POWER_PRESENT:
        return "USB power is already present (computer, charger or powered hub). Unplug it, "
               "wait a few seconds and retry";
    case OTG_E_CHARGER:
        return "TI-OS has not switched the charger off yet. Wait 5 seconds and retry";
    case OTG_E_NO_VBUS:
        return "5 V did not come up with the ID pin floating";
    case OTG_E_FAULT:
        return "the 5 V output reported over-current / VBUS error and was switched off";
    case OTG_E_NO_HOST:
        return "5 V came up but the controller did not become host via HNP";
    case OTG_E_NO_DEVICE:
        return "host mode is up but no device was detected. Is the drive plugged in and powered?";
    case OTG_E_NOT_ENABLED:
        return "the port did not enable after reset";
    default:
        return "unknown error";
    }
}
