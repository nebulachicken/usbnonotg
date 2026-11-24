/* Standalone, non-resident, read-only hook discovery for the six listed OSes.
 * Only the report file is written; no OS hooks or candidate functions are called.
 */
#include <os.h>
#include <nspireio2.h>
#include "../hook_report.h"

extern unsigned int nl_osid(void);

/* UART writes may wait indefinitely when no serial receiver is available. */
#ifndef USB_HOOKSCAN_UART
#define USB_HOOKSCAN_UART 0
#endif

static nio_console console;
static int report_failed;

static int read_sdram(void *context, uint32_t address, void *out, size_t length)
{
    (void)context;

    if (address < 0x10000000 || address >= 0x12000000 || length > 0x12000000 - address) {
        return -1;
    }

    memcpy(out, (const void *)(uintptr_t)address, length);
    return 0;
}

static void emit_report(void *context, const char *line)
{
    FILE *report = context;

    if (fprintf(report, "%s\n", line) < 0) {
        report_failed = 1;
    }
#if USB_HOOKSCAN_UART
    uart_printf("%s\n", line);
#endif
}

static void show_progress(void *context, uint32_t address)
{
    (void)context;
    nio_printf(&console, "Scanning 0x%08x...\n", (unsigned)address);
    nio_DrawConsole(&console);
}

static FILE *open_report(unsigned os_id, char path[128])
{
    unsigned sequence;
    struct stat st;

    for (sequence = 0; sequence < 100; ++sequence) {
        snprintf(path, 128, "/documents/usb-hooks-%u-%02u.tns", os_id, sequence);

        if (stat(path, &st) == 0) {
            continue;
        }
        if (errno != ENOENT) {
            return NULL;
        }

        return fopen(path, "w");
    }

    return NULL;
}

int main(void)
{
    struct usb_hook_matches matches[USB_HOOK_COUNT];
    const struct usb_hook_reader reader = {.read = read_sdram};
    const struct usb_os_profile *profile;
    char report_path[128];
    FILE *report = NULL;
    unsigned os_id = nl_osid();
    unsigned unique = 0;
    size_t id;
    int result = 1;

    nio_InitConsole(&console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, 15, 0);
    profile = usb_os_profile_find(os_id);

    if (!profile || nl_hwsubtype() != 2) {
        nio_printf(&console, "This scanner is for CX II OS IDs 44 through 49.\n");
        goto done;
    }

    nio_printf(&console, "%s (OS id %u)\n", profile->name, os_id);
    nio_printf(&console, "Discovery only. No hooks will be installed.\n");
    nio_DrawConsole(&console);

    report = open_report(os_id, report_path);
    if (!report) {
        nio_printf(&console, "Cannot create report (errno %d).\n", errno);
        goto done;
    }

    if (usb_hook_scan(&reader, profile->scan_begin, profile->scan_end, matches, show_progress,
                      NULL)) {
        emit_report(report, "ERROR: scan did not complete. Do not use partial results.");
        nio_printf(&console, "Scan failed.\n");
        goto close_report;
    }

    nio_printf(&console, "Scan complete. Writing report...\n");
    nio_DrawConsole(&console);
    usb_hook_report(&reader, profile, matches, emit_report, report);

    for (id = 0; id < USB_HOOK_COUNT; ++id) {
        const char *status = matches[id].count == 1 ? "unique"
                             : matches[id].count    ? "AMBIGUOUS"
                                                    : "MISSING";

        nio_printf(&console, "%s: %s (%u)\n", usb_hook_patterns[id].name, status,
                   (unsigned)matches[id].count);
        if (matches[id].count == 1) {
            ++unique;
        }
    }

    nio_printf(&console, "%u/%u hooks unique. Candidates still need ABI review.\n", unique,
               USB_HOOK_COUNT);
    result = 0;

close_report:
    nio_printf(&console, "Closing report...\n");
    nio_DrawConsole(&console);
    if (fclose(report)) {
        report_failed = 1;
    }
    if (report_failed) {
        nio_printf(&console, "Report write failed; results may be incomplete.\n");
        result = 1;
    } else {
        nio_printf(&console, "Report: %s\n", report_path);
    }

done:
    nio_printf(&console, "Press any key to exit.\n");
    nio_DrawConsole(&console);
    wait_no_key_pressed();
    wait_key_pressed();
    return result;
}
