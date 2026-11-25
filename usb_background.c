/*
 * Optional forced-host maintenance worker.
 *
 * TCC_Create_Task is an internal Nucleus function, not a stable Ndless API.
 * This module will not call it unless the selected OS profile supplies a
 * verified entry address, instruction signature, and complete task ABI.
 * The current CX II 6.2.0.333 profiles deliberately leave these unset.
 */
#include <os.h>
#include <stdint.h>
#include <string.h>
#include "usb_background.h"
#include "usb_storage.h"
#include "otg_host.h"
#include "os_runtime.h"
#include "console.h"

#ifndef NU_PREEMPT
#define NU_PREEMPT 10u
#endif
#ifndef NU_START
#define NU_START 12u
#endif

#define USB_BACKGROUND_STACK_SIZE 8192u
/* Placeholder until the target OS priority range is verified. This value is
 * unreachable while task creation remains disabled in the OS profile.
 */
#define USB_BACKGROUND_PRIORITY 255u
#define USB_BACKGROUND_PERIOD_MS 250u

/* Ndless exposes NU_TASK as an opaque type (typedef void NU_TASK), not a
 * concrete Nucleus TCB definition. Reserve aligned opaque storage instead.
 * Task creation remains disabled unless a profile supplies a verified TCB
 * size and ABI; os_runtime.c checks that the size fits this buffer limit.
 */
static uint8_t background_tcb_storage[USB_OS_TASK_TCB_STORAGE_MAX]
    __attribute__((aligned(8)));
static uint8_t background_stack[USB_BACKGROUND_STACK_SIZE] __attribute__((aligned(8)));
static volatile int background_state;
static char background_name[] = "USBKEEP";

static void background_task_entry(unsigned argc, void *argv)
{
    (void)argc;
    (void)argv;
    background_state = 2;

    for (;;) {
        int rc;

        if (!otg_host_active()) {
            background_state = 3;
            for (;;) {
                msleep(1000);
            }
        }

        usb_storage_lock();
        rc = otg_host_background_tick();
        usb_storage_unlock();

        if (rc < 0) {
            /* A definitive health failure tears down the session. Do not touch
             * the controller again; remain resident but dormant.
             */
            background_state = 3;
            for (;;) {
                msleep(1000);
            }
        }

        /* rc == 1 is transient (e.g. the OS did not permit a timer takeover). */
        msleep(USB_BACKGROUND_PERIOD_MS);
    }
}

int usb_background_start(void)
{
    int rc;

    if (!otg_host_active()) {
        return -1;
    }
    if (background_state == 1 || background_state == 2) {
        return 0;
    }
    if (!usb_os_background_task_available()) {
        return 1;
    }

    memset(background_tcb_storage, 0, sizeof(background_tcb_storage));
    memset(background_stack, 0, sizeof(background_stack));
    background_state = 1;

    rc = usb_os_task_create(background_tcb_storage, background_name, background_task_entry,
                            0, NULL, background_stack, sizeof(background_stack),
                            USB_BACKGROUND_PRIORITY, 0, NU_PREEMPT, NU_START);
    if (rc != 0) {
        background_state = 0;
        log("usb-storage: background task creation failed (%d)\n", rc);
        return -1;
    }

    log("usb-storage: background USB maintenance task started\n");
    return 0;
}

int usb_background_is_running(void)
{
    return background_state == 1 || background_state == 2;
}
