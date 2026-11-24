#include "os_profiles.h"

/* A profile becomes runnable only after both its hook addresses and ABI have
 * been reviewed. Zero-filled hooks are intentional, never address guesses.
 * The scanner prints candidate initializers for pending profiles.
 */
const struct usb_os_profile usb_os_profiles[] = {
    {
        .os_id = 44,
        .name = "CX II 6.2.0.333 NCAS",
        .reviewed = 1,
        .abi = USB_OS_ABI_NUCLEUS_CX2_V1,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
        .hooks =
            {
                [USB_HOOK_FINDMOUNT] = 0x10499c38,
                [USB_HOOK_PARSEDRIVE] = 0x10499bcc,
                [USB_HOOK_SEMAPHORE_CREATE] = 0x103a690c,
                [USB_HOOK_SEMAPHORE_OBTAIN] = 0x103a69dc,
                [USB_HOOK_SEMAPHORE_RELEASE] = 0x103a6894,
                [USB_HOOK_ALLOCATE_FSNODE] = 0x104cc054,
                [USB_HOOK_DEALLOCATE_FSNODE] = 0x104cc280,
                [USB_HOOK_FSNODE_TO_STRING] = 0x104cc2e4,
                [USB_HOOK_RENAME] = 0x104cadb8,
            },
        .usb =
            {
                .device_softc_offset = 0x08,
                .device_ivars_offset = 0x14,
                .usb_device_address_offset = 0x08,
                .usb_device_speed_offset = 0x0b,
                .pipe_endpoint_offset = 0x08,
                .endpoint_descriptor_offset = 0x00,
                .high_speed_value = 3,
            },
    },
    {
        .os_id = 45,
        .name = "CX II-T 6.2.0.333",
        .reviewed = 0,
        .abi = USB_OS_ABI_UNREVIEWED,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
    },
    {
        .os_id = 46,
        .name = "CX II 6.2.0.333 CAS",
        .reviewed = 1,
        .abi = USB_OS_ABI_NUCLEUS_CX2_V1,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
        /* Captured in tests/fixtures/usb-hooks-46-01.txt. The live A:
         * rename callback selects the second signature match. Uses the same
         * core ABI and USB layouts as NCAS, as confirmed by the maintainer.
         */
        .hooks =
            {
                [USB_HOOK_FINDMOUNT] = 0x1049ac38,
                [USB_HOOK_PARSEDRIVE] = 0x1049abcc,
                [USB_HOOK_SEMAPHORE_CREATE] = 0x103a7914,
                [USB_HOOK_SEMAPHORE_OBTAIN] = 0x103a79e4,
                [USB_HOOK_SEMAPHORE_RELEASE] = 0x103a789c,
                [USB_HOOK_ALLOCATE_FSNODE] = 0x104cd054,
                [USB_HOOK_DEALLOCATE_FSNODE] = 0x104cd280,
                [USB_HOOK_FSNODE_TO_STRING] = 0x104cd2e4,
                [USB_HOOK_RENAME] = 0x104cbdb8,
            },
        .usb =
            {
                .device_softc_offset = 0x08,
                .device_ivars_offset = 0x14,
                .usb_device_address_offset = 0x08,
                .usb_device_speed_offset = 0x0b,
                .pipe_endpoint_offset = 0x08,
                .endpoint_descriptor_offset = 0x00,
                .high_speed_value = 3,
            },
    },
    {
        .os_id = 47,
        .name = "CX II 6.4.0.74 NCAS",
        .reviewed = 0,
        .abi = USB_OS_ABI_UNREVIEWED,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
    },
    {
        .os_id = 48,
        .name = "CX II-T 6.4.0.74",
        .reviewed = 0,
        .abi = USB_OS_ABI_UNREVIEWED,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
    },
    {
        .os_id = 49,
        .name = "CX II 6.4.0.74 CAS",
        .reviewed = 0,
        .abi = USB_OS_ABI_UNREVIEWED,
        .scan_begin = 0x10300000,
        .scan_end = 0x10500000,
    },
};

const size_t usb_os_profile_count = sizeof(usb_os_profiles) / sizeof(usb_os_profiles[0]);

const struct usb_os_profile *usb_os_profile_find(unsigned os_id)
{
    size_t index;

    for (index = 0; index < usb_os_profile_count; ++index) {
        if (usb_os_profiles[index].os_id == os_id) {
            return &usb_os_profiles[index];
        }
    }

    return NULL;
}
