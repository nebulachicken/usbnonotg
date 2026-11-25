#include "hook_patterns.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned char image[16384];
static const uint32_t base = 0x10000000;

static int read_image(void *context, uint32_t address, void *out, size_t length)
{
    (void)context;

    if (address < base || address - base > sizeof(image) ||
        length > sizeof(image) - (address - base)) {
        return -1;
    }

    memcpy(out, image + address - base, length);
    return 0;
}

static void place_pattern(const struct usb_hook_pattern *pattern, size_t offset)
{
    size_t index;

    for (index = 0; index < pattern->word_count; ++index) {
        uint32_t word = pattern->words[index].value;

        memcpy(image + offset + index * 4, &word, 4);
    }
}

int main(void)
{
    struct usb_hook_matches matches[USB_HOOK_COUNT];
    const struct usb_hook_reader reader = {.read = read_image};
    const struct usb_hook_pattern *pattern = &usb_hook_patterns[USB_HOOK_FINDMOUNT];
    uint32_t target;
    size_t index;

    memset(image, 0xcc, sizeof(image));
    place_pattern(pattern, USB_HOOK_SCAN_BLOCK - 8);
    assert(!usb_hook_scan(&reader, base, base + sizeof(image), matches, NULL, NULL));
    assert(matches[USB_HOOK_FINDMOUNT].count == 1);
    assert(matches[USB_HOOK_FINDMOUNT].addresses[0] == base + USB_HOOK_SCAN_BLOCK - 8);

    place_pattern(pattern, 128);
    assert(!usb_hook_scan(&reader, base, base + sizeof(image), matches, NULL, NULL));
    assert(matches[USB_HOOK_FINDMOUNT].count == 2);
    assert(usb_hook_scan(&reader, base + 1, base + sizeof(image), matches, NULL, NULL) < 0);
    assert(!usb_hook_match_at(&reader, pattern, base + sizeof(image) - 4));

    memset(image, 0xcc, sizeof(image));
    for (index = 0; index < USB_HOOK_MAX_CANDIDATES + 1; ++index) {
        place_pattern(pattern, index * 256);
    }
    assert(!usb_hook_scan(&reader, base, base + sizeof(image), matches, NULL, NULL));
    assert(matches[USB_HOOK_FINDMOUNT].count == USB_HOOK_MAX_CANDIDATES + 1);

    assert(!usb_hook_decode_branch(0x10499c50, 0xebffffdd, &target));
    assert(target == 0x10499bcc);
    assert(!usb_hook_decode_literal(0x10499c64, 0xe59f506c, &target));
    assert(target == 0x10499cd8);
    assert(usb_hook_decode_branch(0, 0xe1a00000, &target) < 0);
    assert(usb_hook_decode_branch(0, 0xeafffffb, &target) < 0);

    for (index = 44; index <= 49; ++index) {
        const struct usb_os_profile *profile = usb_os_profile_find(index);

        assert(profile != NULL);
        assert(profile->reviewed == (index == 44 || index == 46));
        /* No Nucleus task function address or ABI has been verified for the
         * supported CX II images, so background task creation must stay off.
         */
        assert(profile->tcc_create_task == 0);
        assert(profile->tcc_create_task_abi_verified == 0);
        assert(profile->tcc_task_tcb_size == 0);
    }

    puts("hook pattern bounds, block overlap, ambiguity, decoders and OS profiles passed");
    return 0;
}
