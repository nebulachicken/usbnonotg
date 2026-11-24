#ifndef USB_HOOK_PATTERNS_H
#define USB_HOOK_PATTERNS_H

#include "os_profiles.h"

#define USB_HOOK_MAX_WORDS 32
#define USB_HOOK_MAX_CANDIDATES 16
#define USB_HOOK_SCAN_BLOCK 4096

struct usb_hook_word {
    uint32_t value;
    uint32_t mask;
};

struct usb_hook_pattern {
    enum usb_hook_id id;
    const char *name;
    const char *enum_name;
    uint32_t reference_address;
    size_t word_count;
    const struct usb_hook_word *words;
};

/* read() returns zero on success and must reject addresses outside its mapped
 * memory. It must support unaligned destination buffers and never read MMIO.
 */
struct usb_hook_reader {
    int (*read)(void *context, uint32_t address, void *out, size_t length);
    void *context;
};

struct usb_hook_matches {
    size_t count;
    uint32_t addresses[USB_HOOK_MAX_CANDIDATES];
};

typedef void (*usb_hook_progress_fn)(void *context, uint32_t address);

extern const struct usb_hook_pattern usb_hook_patterns[USB_HOOK_COUNT];
extern const char usb_hook_reference_os[];
extern const char usb_hook_reference_sha256[];

int usb_hook_match_words(const struct usb_hook_pattern *pattern, const uint32_t *words,
                         size_t word_count);
int usb_hook_match_at(const struct usb_hook_reader *reader, const struct usb_hook_pattern *pattern,
                      uint32_t address);
int usb_hook_scan(const struct usb_hook_reader *reader, uint32_t begin, uint32_t end,
                  struct usb_hook_matches matches[USB_HOOK_COUNT], usb_hook_progress_fn progress,
                  void *context);
int usb_hook_read_u32(const struct usb_hook_reader *reader, uint32_t address, uint32_t *out);
int usb_hook_decode_branch(uint32_t address, uint32_t instruction, uint32_t *target);
int usb_hook_decode_literal(uint32_t address, uint32_t instruction, uint32_t *literal);

#endif
