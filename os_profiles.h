#ifndef USB_OS_PROFILES_H
#define USB_OS_PROFILES_H

#include <stddef.h>
#include <stdint.h>

enum usb_hook_id {
    USB_HOOK_FINDMOUNT,
    USB_HOOK_PARSEDRIVE,
    USB_HOOK_SEMAPHORE_CREATE,
    USB_HOOK_SEMAPHORE_OBTAIN,
    USB_HOOK_SEMAPHORE_RELEASE,
    USB_HOOK_ALLOCATE_FSNODE,
    USB_HOOK_DEALLOCATE_FSNODE,
    USB_HOOK_FSNODE_TO_STRING,
    USB_HOOK_RENAME,
    USB_HOOK_COUNT
};

enum usb_os_abi { USB_OS_ABI_UNREVIEWED, USB_OS_ABI_NUCLEUS_CX2_V1 };

/* These are USB-stack structure offsets, not filesystem or MMIO addresses. */
struct usb_os_usb_layout {
    uint16_t device_softc_offset;
    uint16_t device_ivars_offset;
    uint16_t usb_device_address_offset;
    uint16_t usb_device_speed_offset;
    uint16_t pipe_endpoint_offset;
    uint16_t endpoint_descriptor_offset;
    uint8_t high_speed_value;
};

struct usb_os_profile {
    unsigned os_id;
    const char *name;
    int reviewed;
    enum usb_os_abi abi;
    uint32_t scan_begin;
    uint32_t scan_end;
    uint32_t hooks[USB_HOOK_COUNT];
    /* Optional TCC_Create_Task entry point. Zero means unsupported.
     * This private Nucleus API must be verified separately per OS image.
     */
    uint32_t tcc_create_task;
    uint32_t tcc_create_task_sig[2];
    uint32_t tcc_create_task_mask[2];
    /* Must be set only after the full call ABI and TCB size are verified. */
    uint32_t tcc_create_task_abi_verified;
    uint32_t tcc_task_tcb_size;
    struct usb_os_usb_layout usb;
};

extern const struct usb_os_profile usb_os_profiles[];
extern const size_t usb_os_profile_count;
const struct usb_os_profile *usb_os_profile_find(unsigned os_id);

#endif
