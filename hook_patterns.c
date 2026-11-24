#include "hook_patterns.h"
#include <limits.h>
#include <string.h>

#include "hooks/patterns.inc"

int usb_hook_match_words(const struct usb_hook_pattern *pattern, const uint32_t *words,
                         size_t word_count)
{
    size_t index;

    if (!pattern || !words || !pattern->word_count || word_count < pattern->word_count) {
        return 0;
    }

    for (index = 0; index < pattern->word_count; ++index) {
        if ((words[index] & pattern->words[index].mask) != pattern->words[index].value) {
            return 0;
        }
    }

    return 1;
}

int usb_hook_match_at(const struct usb_hook_reader *reader, const struct usb_hook_pattern *pattern,
                      uint32_t address)
{
    uint32_t words[USB_HOOK_MAX_WORDS];

    if (!reader || !reader->read || !pattern || (address & 3) ||
        pattern->word_count > USB_HOOK_MAX_WORDS) {
        return 0;
    }

    if (reader->read(reader->context, address, words, pattern->word_count * sizeof(uint32_t))) {
        return 0;
    }

    return usb_hook_match_words(pattern, words, pattern->word_count);
}

int usb_hook_scan(const struct usb_hook_reader *reader, uint32_t begin, uint32_t end,
                  struct usb_hook_matches matches[USB_HOOK_COUNT], usb_hook_progress_fn progress,
                  void *context)
{
    /* Extra words let patterns straddle a block boundary without being missed. */
    uint32_t words[USB_HOOK_SCAN_BLOCK / 4 + USB_HOOK_MAX_WORDS - 1];
    uint32_t address;

    if (!reader || !reader->read || !matches || begin >= end || ((begin | end) & 3)) {
        return -1;
    }

    memset(matches, 0, sizeof(*matches) * USB_HOOK_COUNT);

    for (address = begin; address < end;) {
        size_t remaining = end - address;
        size_t bytes = remaining < sizeof(words) ? remaining : sizeof(words);
        size_t step = remaining < USB_HOOK_SCAN_BLOCK ? remaining : USB_HOOK_SCAN_BLOCK;
        size_t offset;

        if (reader->read(reader->context, address, words, bytes)) {
            return -1;
        }

        for (offset = 0; offset < step; offset += sizeof(uint32_t)) {
            size_t id;

            for (id = 0; id < USB_HOOK_COUNT; ++id) {
                const struct usb_hook_pattern *pattern = &usb_hook_patterns[id];

                if (usb_hook_match_words(pattern, words + offset / 4, (bytes - offset) / 4)) {
                    struct usb_hook_matches *result = &matches[id];

                    if (result->count < USB_HOOK_MAX_CANDIDATES) {
                        result->addresses[result->count] = address + offset;
                    }
                    ++result->count;
                }
            }
        }

        if (progress && !((address - begin) & 0x3ffff)) {
            progress(context, address);
        }
        address += step;
    }

    return 0;
}

int usb_hook_read_u32(const struct usb_hook_reader *reader, uint32_t address, uint32_t *out)
{
    if (!reader || !reader->read || !out || (address & 3)) {
        return -1;
    }

    return reader->read(reader->context, address, out, sizeof(*out));
}

int usb_hook_decode_branch(uint32_t address, uint32_t instruction, uint32_t *target)
{
    int32_t displacement;
    int64_t destination;

    if (!target || (instruction & 0x0e000000) != 0x0a000000 ||
        (instruction & 0xf0000000) == 0xf0000000) {
        return -1;
    }

    displacement = (int32_t)(instruction & 0x00ffffff);
    if (displacement & 0x00800000) {
        displacement -= 0x01000000;
    }
    destination = (int64_t)address + 8 + (int64_t)displacement * 4;

    if (destination < 0 || destination > UINT32_MAX) {
        return -1;
    }

    *target = (uint32_t)destination;
    return 0;
}

int usb_hook_decode_literal(uint32_t address, uint32_t instruction, uint32_t *literal)
{
    int64_t displacement = instruction & 0xfff;
    int64_t destination;

    if (!literal || (instruction & 0x0f7f0000) != 0x051f0000) {
        return -1;
    }

    if (!(instruction & (1u << 23))) {
        displacement = -displacement;
    }
    destination = (int64_t)address + 8 + displacement;

    if (destination < 0 || destination > UINT32_MAX || (destination & 3)) {
        return -1;
    }

    *literal = (uint32_t)destination;
    return 0;
}
