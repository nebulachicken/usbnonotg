#ifndef FOTG210_H
#define FOTG210_H

/* FOTG210 Register Offsets */

#include <stdint.h>

struct fotg210_hcd_regs {
    uint32_t hciversion_caplength; /* 0x00 */
    uint32_t hcsparams;            /* 0x04 */
    uint32_t hccparams;            /* 0x08 */
    uint32_t portroute;            /* 0x0C */

    uint32_t command;             /* 0x10 */
    uint32_t status;              /* 0x14 */
    uint32_t interrupt_enable;    /* 0x18 */
    uint32_t frame_index;         /* 0x1C */
    uint32_t reserved1;           /* 0x20 */
    uint32_t periodic_list_base;  /* 0x24 */
    uint32_t async_list_addr;     /* 0x28 */
    uint32_t reserved2[1];        /* 0x2C */
    uint32_t port_status_control; /* 0x30 */

    uint32_t reserved3[3]; /* 0x34 - 0x3C */
    uint32_t unknown_reg;  /* 0x40 (Op Base + 0x30) */

    uint32_t reserved4[15];        /* 0x44 - 0x7C */
    uint32_t otg_status_control;   /* 0x80 (Op Base + 0x70) */
    uint32_t otg_interrupt_status; /* 0x84 (Op Base + 0x74) */

    uint32_t reserved5[15];         /* 0x88 - 0xC0 */
    uint32_t global_interrupt_mask; /* 0xC4 (Op Base + 0xB4) */

    uint32_t reserved6[149]; /* 0xC8 - 0x328 */
    uint32_t misc;           /* 0x32C (Op Base + 0x31C) */
    uint32_t misc2;          /* 0x330 (Op Base + 0x320) */
};

/* USBCMD Register Bits */
#define CMD_RUN (1 << 0)               /* 1=Run, 0=Stop */
#define CMD_RESET (1 << 1)             /* Core Reset */
#define CMD_FRAME_SIZE_1024 (0x0 << 2) /* 1024 elements in periodic frame list */
#define CMD_FRAME_SIZE_512 (0x1 << 2)  /* 512 elements */
#define CMD_FRAME_SIZE_256 (0x2 << 2)  /* 256 elements */
#define CMD_PSE (1 << 4)               /* Periodic Schedule Enable */
#define CMD_ASE (1 << 5)               /* Asynchronous Schedule Enable */
#define CMD_IAAD (1 << 6)              /* Interrupt on Async Advance Doorbell */
#define CMD_LIGHT_RESET (1 << 7)       /* Light Core Reset (Optional) */
#define CMD_PARK_ENABLE (1 << 11)      /* Async Park Mode Enable */

/* Interrupt Threshold Control shifting (Bits 16-23) */
#define CMD_ITC_SHIFT 16
#define CMD_ITC_IMMEDIATE (0x00 << CMD_ITC_SHIFT) /* Interrupt immediately */
#define CMD_ITC_1_UFRAME (0x01 << CMD_ITC_SHIFT)  /* 1 microframe (125us) */
#define CMD_ITC_2_UFRAMES (0x02 << CMD_ITC_SHIFT) /* 2 microframes (250us) */
#define CMD_ITC_4_UFRAMES (0x04 << CMD_ITC_SHIFT) /* 4 microframes (500us) */
#define CMD_ITC_8_UFRAMES (0x08 << CMD_ITC_SHIFT) /* 8 microframes (1ms default) */

#define STS_INT (1U << 0)
#define STS_ERR (1U << 1)
#define STS_PCD (1U << 2)
#define STS_FATAL (1U << 4)
#define STS_IAA (1U << 5)
#define STS_HALT (1U << 12)
#define STS_ASS (1U << 15)
#define STS_PSS (1U << 14)

#define INTR_USBINT (1 << 0)         /* Transfer Completion Interrupt */
#define INTR_ERROR (1 << 1)          /* Transaction Error Interrupt */
#define INTR_PORT_CHANGE (1 << 2)    /* Port Connect/Disconnect Detect */
#define INTR_FRAME_ROLLOVER (1 << 3) /* Frame List Rollover Event */
#define INTR_HOST_ERR (1 << 4)       /* DMA Host Bus Error System Panic */
#define INTR_ASYNC_ADVANCE (1 << 5)  /* Async Schedule Advance Acknowledge */

#define QTD_TOGGLE (1U << 31)
#define QTD_LENGTH(tok) (((tok) >> 16) & 0x7FFF)
#define QTD_IOC (1U << 15)
#define QTD_CERR(n) ((n) << 10)
#define QTD_PID_OUT (0U << 8)
#define QTD_PID_IN (1U << 8)
#define QTD_PID_SETUP (2U << 8)
#define QTD_STS_ACTIVE (1U << 7)
#define QTD_STS_HALT (1U << 6)
#define QTD_STS_DBE (1U << 5)
#define QTD_STS_BABBLE (1U << 4)
#define QTD_STS_XACT (1U << 3)
#define QTD_STS_MMF (1U << 2)
#define QTD_STS_STS (1U << 1)
#define QTD_STS_PING (1U << 0)

#endif
