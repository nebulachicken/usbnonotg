/* FOTG210 high-speed bulk transfers with temporary asynchronous-schedule ownership. */

#include <os.h>
#include <stdbool.h>
#include "fotg210.h"
#include "console.h"
#include <stdint.h>
#include <usbdi.h>
#include <nucleus.h>
#include <string.h>
#include "usb_storage.h"
#include "os_runtime.h"

static volatile struct fotg210_hcd_regs *hcd = (volatile struct fotg210_hcd_regs *)0xB0000000;

#define PL190_INT_ENABLE (*(volatile uint32_t *)0xDC000010)
#define PL190_ENABLE_INT(vec) (PL190_INT_ENABLE = (1UL << (vec)))
#define PL190_DISABLE_INT(vec) (*(volatile uint32_t *)0xDC000014 = (1UL << (vec)))

#define USB_IRQ_VEC 9
#define USB_DIR_IN 0x80

struct fotg_saved_state {
    uint32_t command;
    uint32_t interrupt_enable;
    uint32_t periodic_base;
    uint32_t async_list_addr;
    uint32_t global_mask;
    uint32_t pl190_enable;
};

/* Standard EHCI Queue Element Transfer Descriptor (qTD) */
struct ehci_qtd {
    volatile uint32_t next_qtd;     /* Next TD pointer (Physical) */
    volatile uint32_t alt_next_qtd; /* Alternate next TD pointer (typically 1 = Terminate) */
    volatile uint32_t token;        /* Total bytes, PID code, Status flags */
    volatile uint32_t buffer[5];    /* 5 Data buffer pointers (4KB page aligned) */
} __attribute__((aligned(32)));

/* Standard EHCI Queue Head (QH) */
struct ehci_qh {
    volatile uint32_t qh_link;       /* Horizontal link pointer to next QH (Physical) */
    volatile uint32_t endpoint_cap1; /* Device address, endpoint number, max packet size */
    volatile uint32_t endpoint_cap2; /* Hub tracking / Multiplier fields */
    volatile uint32_t current_qtd;   /* Overlay: Current TD pointer */
    /* The remaining 16 bytes mirror a qTD overlay space managed by hardware */
    volatile uint32_t overlay_next;
    volatile uint32_t overlay_alt;
    volatile uint32_t overlay_token;
    volatile uint32_t overlay_buffer[5];
} __attribute__((aligned(32)));

struct bulk_toggle_state {
    uint8_t valid;
    uint8_t toggle;
};

static struct bulk_toggle_state endpoint_toggles[128][2][16];

#define EHCI_LINK_TYPE_QH 0x2
#define EHCI_LINK_TERMINATE 0x1
#define QTD_ERROR_MASK (QTD_STS_HALT | QTD_STS_DBE | QTD_STS_BABBLE | QTD_STS_XACT | QTD_STS_MMF)

#define QH_DEVADDR(addr) ((uint32_t)(addr) & 0x7f)
#define QH_ENDPOINT(ep) (((uint32_t)(ep) & 0xf) << 8)
#define QH_EPS_HIGH_SPEED (2U << 12)
#define QH_DTC (1U << 14)
#define QH_HEAD (1U << 15)
#define QH_MAX_PACKET(size) (((uint32_t)(size) & 0x7ff) << 16)
#define QH_NAK_RELOAD(n) (((uint32_t)(n) & 0xf) << 28)
#define QH_MULT(n) (((uint32_t)(n) & 0x3) << 30)

static int wait_schedule(int active)
{
    unsigned guard = 2000000;
    while (!!(hcd->status & STS_ASS) != active && --guard) {
    }
    return guard ? 0 : -1;
}

static int fotg_prepare(struct fotg_saved_state *state)
{
    int old_mask = TCT_Local_Control_Interrupts(-1);

    state->interrupt_enable = hcd->interrupt_enable;
    state->pl190_enable = PL190_INT_ENABLE;
    hcd->interrupt_enable = 0;

    state->global_mask = hcd->global_interrupt_mask;
    state->command = hcd->command;
    state->periodic_base = hcd->periodic_list_base;
    state->async_list_addr = hcd->async_list_addr;

    hcd->command &= ~CMD_ASE;
    int result = wait_schedule(0);

    TCT_Local_Control_Interrupts(old_mask);
    return result;
}

static int fotg_restore(const struct fotg_saved_state *state)
{
    int old_mask = TCT_Local_Control_Interrupts(-1);

    hcd->interrupt_enable = 0;
    hcd->command &= ~CMD_ASE;
    if (wait_schedule(0)) {
        TCT_Local_Control_Interrupts(old_mask);
        return -1;
    }

    hcd->periodic_list_base = state->periodic_base;
    hcd->async_list_addr = state->async_list_addr;
    hcd->global_interrupt_mask = state->global_mask;

    hcd->interrupt_enable = state->interrupt_enable;
    hcd->command = state->command;
    if (state->pl190_enable & (1UL << USB_IRQ_VEC)) {
        PL190_ENABLE_INT(USB_IRQ_VEC);
    } else {
        PL190_DISABLE_INT(USB_IRQ_VEC);
    }

    TCT_Local_Control_Interrupts(old_mask);
    return 0;
}

static void build_minimal_qtd(struct ehci_qtd *qtd, void *buf, uint32_t len, bool is_in_transfer,
                              uint8_t data_toggle)
{
    int i;
    memset(qtd, 0, sizeof(*qtd));
    qtd->alt_next_qtd = EHCI_LINK_TERMINATE;
    qtd->next_qtd = EHCI_LINK_TERMINATE;

    uintptr_t phys_addr = (uintptr_t)buf;
    qtd->buffer[0] = phys_addr;
    for (i = 1; i < 5; i++) {
        qtd->buffer[i] = (phys_addr + (i * 4096)) & 0xFFFFF000;
    }

    uint32_t pid = is_in_transfer ? 1 : 0;
    qtd->token = (len << 16) | (data_toggle ? QTD_TOGGLE : 0) | QTD_IOC | QTD_CERR(3) | (pid << 8) |
                 QTD_STS_ACTIVE;
}

static void build_minimal_qh(struct ehci_qh *qh, struct ehci_qtd *target_qtd, int dev_addr,
                             int ep_num, uint16_t max_packet)
{
    uintptr_t qh_phys = (uintptr_t)qh;
    uintptr_t qtd_phys = (uintptr_t)target_qtd;

    memset(qh, 0, sizeof(*qh));
    qh->qh_link = qh_phys | EHCI_LINK_TYPE_QH;
    qh->endpoint_cap1 = QH_MAX_PACKET(max_packet) | QH_NAK_RELOAD(4) | QH_HEAD | QH_DTC |
                        QH_EPS_HIGH_SPEED | QH_ENDPOINT(ep_num) | QH_DEVADDR(dev_addr);
    qh->endpoint_cap2 = QH_MULT(1);
    qh->current_qtd = 0;
    qh->overlay_next = qtd_phys;
    qh->overlay_alt = EHCI_LINK_TERMINATE;
    qh->overlay_token = 0;
}

static uint32_t timeout_to_frame_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return 8;
    }
    if (timeout_ms > 0x1fff / 8) {
        return 0x1fff;
    }
    return timeout_ms * 8;
}

static bool transfer_finished(uint32_t token)
{
    return (token & QTD_STS_ACTIVE) == 0;
}

static uint32_t transferred_len(uint32_t len, uint32_t token)
{
    uint32_t remaining = QTD_LENGTH(token);
    return remaining < len ? len - remaining : 0;
}

static bool timeout_expired(uint32_t start_frame, uint32_t timeout_ticks)
{
    uint32_t elapsed = (hcd->frame_index - start_frame) & 0x3fff;
    return elapsed >= timeout_ticks;
}

static uint8_t get_endpoint_toggle(uint8_t addr, uint8_t endp_addr)
{
    uint8_t ep_num = endp_addr & 0x0f;
    uint8_t dir = (endp_addr & USB_DIR_IN) ? 1 : 0;
    struct bulk_toggle_state *state = &endpoint_toggles[addr & 0x7f][dir][ep_num];

    if (!state->valid) {
        state->valid = 1;
        state->toggle = 0;
    }

    return state->toggle;
}

static void set_endpoint_toggle(uint8_t addr, uint8_t endp_addr, uint32_t token)
{
    uint8_t ep_num = endp_addr & 0x0f;
    uint8_t dir = (endp_addr & USB_DIR_IN) ? 1 : 0;
    struct bulk_toggle_state *state = &endpoint_toggles[addr & 0x7f][dir][ep_num];

    state->valid = 1;
    state->toggle = (token & QTD_TOGGLE) ? 1 : 0;
}

void usb_bulk_reset_toggles(usbd_device_handle dev)
{
    const struct usb_os_usb_layout *layout = &usb_os_active_profile()->usb;
    uint8_t addr = *(uint8_t *)((uintptr_t)dev + layout->usb_device_address_offset);
    memset(endpoint_toggles[addr & 127], 0, sizeof(endpoint_toggles[0]));
}

/* Resident DMA storage is never freed while a malfunctioning controller may own it. */
static uint8_t dma_qh[sizeof(struct ehci_qh) + 31];
static uint8_t dma_qtd[sizeof(struct ehci_qtd) + 31];
static uint8_t dma_buffer[16384 + 4095];
static int controller_failed;

int usbd_do_bulk_transfer(usbd_device_handle dev, usbd_pipe_handle ep, void *buf, uint32_t len,
                          uint32_t timeout_ms)
{
    int result = -1, old_mask;
    unsigned guard = 20000000;
    if (controller_failed || !dev || !ep || !buf || !len || len > 16384) {
        return -1;
    }
    struct fotg_saved_state state;
    const struct usb_os_usb_layout *layout = &usb_os_active_profile()->usb;
    uint8_t addr = *(uint8_t *)((uintptr_t)dev + layout->usb_device_address_offset);
    uint8_t dev_speed = *(uint8_t *)((uintptr_t)dev + layout->usb_device_speed_offset);
    void *endpoint_struct = *(void **)((uintptr_t)ep + layout->pipe_endpoint_offset);
    uint8_t *edesc = *(uint8_t **)((uintptr_t)endpoint_struct + layout->endpoint_descriptor_offset);
    uint8_t endp_addr = edesc[2];
    uint8_t raw_endp_addr = endp_addr;
    uint16_t max_packet = (edesc[4] | ((uint16_t)edesc[5] << 8)) & 0x7ff;
    bool is_in_transfer = (endp_addr & USB_DIR_IN) != 0;
    uint8_t data_toggle = get_endpoint_toggle(addr, raw_endp_addr);
    struct ehci_qh *qh = (void *)(((uintptr_t)dma_qh + 31) & ~(uintptr_t)31);
    struct ehci_qtd *qtd = (void *)(((uintptr_t)dma_qtd + 31) & ~(uintptr_t)31);
    void *buf_aligned = (void *)(((uintptr_t)dma_buffer + 4095) & ~(uintptr_t)4095);
    uint32_t token = 0;
    uint32_t timeout_ticks = timeout_to_frame_ticks(timeout_ms);
    uint32_t start_frame = 0;
    uint32_t initial_token = 0;
    uint32_t endpoint_cap1 = 0;
    bool completed = false;

    /* This controller path does not implement hub split transactions. */
    if (!max_packet || dev_speed != layout->high_speed_value) {
        return -1;
    }
    if (!is_in_transfer) {
        memcpy(buf_aligned, buf, len);
    }

    old_mask = TCT_Local_Control_Interrupts(-1);
    if (fotg_prepare(&state)) {
        controller_failed = 1;
        goto restore;
    }
    PL190_DISABLE_INT(USB_IRQ_VEC);
    hcd->interrupt_enable = 0;

    endp_addr &= 0x0F;
    build_minimal_qtd(qtd, buf_aligned, len, is_in_transfer, data_toggle);
    build_minimal_qh(qh, qtd, addr, endp_addr, max_packet);
    initial_token = qtd->token;
    endpoint_cap1 = qh->endpoint_cap1;
    clear_cache(); /* Publish descriptors and OUT data before starting DMA. */

    hcd->async_list_addr = (uint32_t)qh;
    hcd->command = (state.command & ~(CMD_ASE | CMD_IAAD)) | CMD_RUN | CMD_ASE;
    if (wait_schedule(1)) {
        goto restore;
    }
    start_frame = hcd->frame_index;

    for (;;) {
        clear_cache();
        token = qtd->token;
        if (transfer_finished(token)) {
            completed = true;
            break;
        }

        if (timeout_expired(start_frame, timeout_ticks) || !--guard || (hcd->status & STS_FATAL)) {
            break;
        }
    }

    if (!completed) {
        token = qtd->token;
        log("bulk hijack timeout token=0x%x sts=0x%x\n", (unsigned)token, (unsigned)hcd->status);
    } else if (token & QTD_ERROR_MASK) {
        log("bulk hijack error token=0x%x init=0x%x sts=0x%x cap1=0x%x speed=%u ep=0x%x mps=%u\n",
            (unsigned)token, (unsigned)initial_token, (unsigned)hcd->status,
            (unsigned)endpoint_cap1, (unsigned)dev_speed, (unsigned)raw_endp_addr,
            (unsigned)max_packet);
    } else if (is_in_transfer) {
        uint32_t done = transferred_len(len, token);
        uint32_t packets = done / max_packet + ((done % max_packet || done < len) ? 1 : 0);
        set_endpoint_toggle(addr, raw_endp_addr, (data_toggle ^ (packets & 1)) ? QTD_TOGGLE : 0);
        memcpy(buf, buf_aligned, done);
        result = done;
    } else {
        uint32_t done = transferred_len(len, token);
        uint32_t packets = (done + max_packet - 1) / max_packet;
        set_endpoint_toggle(addr, raw_endp_addr, (data_toggle ^ (packets & 1)) ? QTD_TOGGLE : 0);
        result = done;
    }

restore:
    if (fotg_restore(&state)) {
        controller_failed = 1;
        result = -1;
    }
    TCT_Local_Control_Interrupts(old_mask);
    return result;
}
