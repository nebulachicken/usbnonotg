#include "hook_report.h"
#include <stdarg.h>
#include <stdio.h>

static void report_line(usb_hook_log_fn emit, void *context, const char *format, ...)
{
    char line[256];
    va_list args;

    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    emit(context, line);
}

static int contains(const struct usb_hook_matches *matches, uint32_t address)
{
    size_t index;

    for (index = 0; index < matches->count && index < USB_HOOK_MAX_CANDIDATES; ++index) {
        if (matches->addresses[index] == address) {
            return 1;
        }
    }

    return 0;
}

static void report_relationships(const struct usb_hook_reader *reader,
                                 const struct usb_hook_matches matches[USB_HOOK_COUNT],
                                 usb_hook_log_fn emit, void *context)
{
    static const struct {
        enum usb_hook_id id;
        unsigned slot;
    } callbacks[] = {{USB_HOOK_ALLOCATE_FSNODE, 24},
                     {USB_HOOK_DEALLOCATE_FSNODE, 25},
                     {USB_HOOK_FSNODE_TO_STRING, 26},
                     {USB_HOOK_RENAME, 17}};

    uint32_t findmount;
    uint32_t instruction;
    uint32_t parsedrive;
    uint32_t literal;
    uint32_t table;
    unsigned index;

    report_line(emit, context, "RELATIONSHIPS (reference instruction/structure layout only)");

    if (matches[USB_HOOK_FINDMOUNT].count != 1) {
        report_line(emit, context, "Skipped: findmount is missing or ambiguous.");
        return;
    }

    findmount = matches[USB_HOOK_FINDMOUNT].addresses[0];

    if (usb_hook_read_u32(reader, findmount + 0x18, &instruction) ||
        usb_hook_decode_branch(findmount + 0x18, instruction, &parsedrive)) {
        report_line(emit, context, "Cannot decode reference-layout parsedrive BL at +0x18.");
        return;
    }

    report_line(emit, context, "findmount BL +0x18 -> parsedrive 0x%08x", (unsigned)parsedrive);
    report_line(emit, context, "parsedrive relation matches scan: %s",
                contains(&matches[USB_HOOK_PARSEDRIVE], parsedrive) ? "yes" : "NO");

    if (usb_hook_read_u32(reader, findmount + 0x2c, &instruction) ||
        usb_hook_decode_literal(findmount + 0x2c, instruction, &literal) ||
        usb_hook_read_u32(reader, literal, &table)) {
        report_line(emit, context, "Cannot decode reference-layout mount-table LDR at +0x2c.");
        return;
    }

    /* Do not allow corrupt pointer arithmetic to wrap into another mapped area. */
    if ((table & 3) || table > UINT32_MAX - 20 * 0x28) {
        report_line(emit, context, "Invalid mount-table pointer.");
        return;
    }

    report_line(emit, context, "findmount literal 0x%08x -> mount table 0x%08x", (unsigned)literal,
                (unsigned)table);

    for (index = 0; index < 20; ++index) {
        uint32_t mount = table + index * 0x28;
        uint32_t flags;
        uint32_t letter_and_drive;
        uint32_t ops;
        unsigned slot;

        if (usb_hook_read_u32(reader, mount + 0x0c, &flags) ||
            usb_hook_read_u32(reader, mount + 0x14, &letter_and_drive)) {
            report_line(emit, context, "Mount-table read is outside the allowed SDRAM range.");
            return;
        }

        if (!(flags & 1) || (letter_and_drive & 0xffff) != 0) {
            continue;
        }

        if (usb_hook_read_u32(reader, mount, &ops) || (ops & 3) || ops > UINT32_MAX - 29 * 4) {
            report_line(emit, context, "Invalid A: callback-table pointer.");
            return;
        }

        report_line(emit, context, "A mount 0x%08x, drive %u, ops 0x%08x", (unsigned)mount,
                    (unsigned)(letter_and_drive >> 16), (unsigned)ops);

        for (slot = 0; slot < 29; ++slot) {
            uint32_t value;

            if (usb_hook_read_u32(reader, ops + slot * 4, &value)) {
                report_line(emit, context, "Callback-table read is outside allowed SDRAM.");
                return;
            }
            report_line(emit, context, "  ops[%02u] = 0x%08x", slot, (unsigned)value);
        }

        for (slot = 0; slot < sizeof(callbacks) / sizeof(callbacks[0]); ++slot) {
            uint32_t value;
            enum usb_hook_id id = callbacks[slot].id;

            if (!usb_hook_read_u32(reader, ops + callbacks[slot].slot * 4, &value)) {
                report_line(emit, context, "  %s relation matches scan: %s",
                            usb_hook_patterns[id].name,
                            contains(&matches[id], value) ? "yes" : "NO");
            }
        }

        return;
    }

    report_line(emit, context, "No active A mount in the reference-layout table.");
}

void usb_hook_report(const struct usb_hook_reader *reader, const struct usb_os_profile *profile,
                     const struct usb_hook_matches matches[USB_HOOK_COUNT], usb_hook_log_fn emit,
                     void *context)
{
    size_t id;

    report_line(emit, context, "usb-storage hook scan, format 1");
    report_line(emit, context, "os_id = %u", profile->os_id);
    report_line(emit, context, "os = %s", profile->name);
    report_line(emit, context, "scan = [0x%08x, 0x%08x)", (unsigned)profile->scan_begin,
                (unsigned)profile->scan_end);
    report_line(emit, context, "Reference signatures: %s", usb_hook_reference_os);
    report_line(emit, context, "Reference SHA256: %s", usb_hook_reference_sha256);
    report_line(emit, context,
                "Candidates are not ABI verification. No candidate code was executed.");
    report_line(emit, context,
                "Matching rule: (ARM LE instruction & mask) == value, 4-byte aligned.");
    report_line(emit, context, "Masks ignore branch destinations and literal-load displacements.");
    report_line(emit, context, "Run after a reboot, before installing usb-storage or other hooks.");

    for (id = 0; id < USB_HOOK_COUNT; ++id) {
        const struct usb_hook_pattern *pattern = &usb_hook_patterns[id];
        const struct usb_hook_matches *result = &matches[id];
        const char *status = result->count == 1 ? "UNIQUE"
                             : result->count    ? "AMBIGUOUS"
                                                : "MISSING";
        size_t index;

        report_line(emit, context, "");
        report_line(emit, context, "HOOK %s [%s] count=%u", pattern->name, status,
                    (unsigned)result->count);
        report_line(emit, context, "reference_address = 0x%08x",
                    (unsigned)pattern->reference_address);
        report_line(emit, context, "pattern value/mask:");

        for (index = 0; index < pattern->word_count; ++index) {
            report_line(emit, context, "  +0x%02x: %08x/%08x", (unsigned)(index * 4),
                        (unsigned)pattern->words[index].value,
                        (unsigned)pattern->words[index].mask);
        }

        for (index = 0; index < result->count && index < USB_HOOK_MAX_CANDIDATES; ++index) {
            uint32_t address = result->addresses[index];
            size_t word;

            report_line(emit, context, "candidate = 0x%08x", (unsigned)address);

            for (word = 0; word < pattern->word_count; ++word) {
                uint32_t value;

                if (usb_hook_read_u32(reader, address + word * 4, &value)) {
                    report_line(emit, context, "  Candidate read failed.");
                    break;
                }
                report_line(emit, context, "  actual +0x%02x: %08x", (unsigned)(word * 4),
                            (unsigned)value);
            }
        }

        if (result->count > USB_HOOK_MAX_CANDIDATES) {
            report_line(emit, context, "Additional addresses omitted; match count is complete.");
        }
    }

    report_line(emit, context, "");
    report_relationships(reader, matches, emit, context);
    report_line(emit, context, "");
    report_line(emit, context, "PROFILE CANDIDATES -- merge into os_profiles.c only after review");
    report_line(emit, context, "{");
    report_line(emit, context, "    .os_id = %u,", profile->os_id);
    report_line(emit, context, "    .name = \"%s\",", profile->name);
    report_line(emit, context, "    .reviewed = 0,");
    report_line(emit, context, "    .abi = USB_OS_ABI_UNREVIEWED,");
    report_line(emit, context, "    .hooks = {");

    for (id = 0; id < USB_HOOK_COUNT; ++id) {
        uint32_t address = matches[id].count == 1 ? matches[id].addresses[0] : 0;

        report_line(emit, context, "        [%s] = 0x%08x, /* %u matches */",
                    usb_hook_patterns[id].enum_name, (unsigned)address,
                    (unsigned)matches[id].count);
    }

    report_line(emit, context, "    }");
    report_line(emit, context,
                "    /* Review USB offsets, callback signatures, DSTAT and mount layout. */");
    report_line(emit, context, "}");
}
