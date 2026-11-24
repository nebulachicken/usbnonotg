#include <os.h>
#include "os_runtime.h"
#include "hook_patterns.h"
#include "console.h"

static const struct usb_os_profile *active_profile;

static int read_sdram(void *context, uint32_t address, void *out, size_t length)
{
    (void)context;

    if (address < 0x10000000 || address >= 0x12000000 || length > 0x12000000 - address) {
        return -1;
    }

    memcpy(out, (const void *)(uintptr_t)address, length);
    return 0;
}

int usb_os_select(unsigned os_id)
{
    const struct usb_os_profile *profile = usb_os_profile_find(os_id);
    const struct usb_hook_reader reader = {.read = read_sdram};
    size_t id;

    active_profile = NULL;

    if (!profile) {
        log("usb-storage: unknown OS id %u\n", os_id);
        return -1;
    }

    log("usb-storage: OS profile %s (id %u)\n", profile->name, profile->os_id);

    if (!profile->reviewed || profile->abi != USB_OS_ABI_NUCLEUS_CX2_V1) {
        log("usb-storage: profile pending hook/ABI review; run usb-hookscan\n");
        return -1;
    }

    if (!profile->usb.high_speed_value) {
        log("usb-storage: incomplete USB structure layout\n");
        return -1;
    }

    for (id = 0; id < USB_HOOK_COUNT; ++id) {
        uint32_t address = profile->hooks[id];
        int matches = usb_hook_match_at(&reader, &usb_hook_patterns[id], address);

        log("usb-storage: hook %s = 0x%08x, pattern %s\n", usb_hook_patterns[id].name,
            (unsigned)address, matches ? "matches" : "FAILED");

        if (!matches) {
            return -1;
        }
    }

    active_profile = profile;
    return 0;
}

const struct usb_os_profile *usb_os_active_profile(void)
{
    return active_profile;
}

int usb_os_parse_drive(const char *path, int allow_current)
{
    typedef int (*parse_drive_fn)(const char *, int);
    parse_drive_fn parse = (parse_drive_fn)(uintptr_t)active_profile->hooks[USB_HOOK_PARSEDRIVE];

    return parse(path, allow_current);
}

int usb_os_semaphore_create(void *semaphore, const char *name, unsigned count, unsigned order)
{
    typedef int (*create_fn)(void *, const char *, unsigned, unsigned);
    create_fn create = (create_fn)(uintptr_t)active_profile->hooks[USB_HOOK_SEMAPHORE_CREATE];

    return create(semaphore, name, count, order);
}

int usb_os_semaphore_obtain(void *semaphore, unsigned timeout)
{
    typedef int (*obtain_fn)(void *, unsigned);
    obtain_fn obtain = (obtain_fn)(uintptr_t)active_profile->hooks[USB_HOOK_SEMAPHORE_OBTAIN];

    return obtain(semaphore, timeout);
}

int usb_os_semaphore_release(void *semaphore)
{
    typedef int (*release_fn)(void *);
    release_fn release = (release_fn)(uintptr_t)active_profile->hooks[USB_HOOK_SEMAPHORE_RELEASE];

    return release(semaphore);
}
