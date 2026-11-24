#include <os.h>
#include <nspireio2.h>
#include <stdarg.h>
#include "console.h"

/* Serial transmission can block without a receiver. Enable explicitly when
 * collecting resident USB callback logs through a working UART connection. */
#ifndef USB_STORAGE_UART
#define USB_STORAGE_UART 0
#endif

static nio_console console;
static int screen_logging;
static NU_TASK *startup_task;

int usb_console_begin(void)
{
    if (!nio_InitConsole(&console, NIO_MAX_COLS, NIO_MAX_ROWS, 0, 0, 15, 0)) {
        return -1;
    }
    startup_task = TCC_Current_Task_Pointer();
    screen_logging = 1;
    return 0;
}

void usb_console_end(void)
{
    screen_logging = 0;
    startup_task = NULL;
    nio_CleanUp(&console);
}

void usb_log(const char *format, ...)
{
    char message[512];
    va_list args;
    /* USB callbacks may run on another task while the startup screen is open.
     * Only the launcher task owns this console and may draw or free it. */
    int draw = screen_logging && TCC_Current_Task_Pointer() == startup_task;

#if !USB_STORAGE_UART
    if (!draw) {
        return;
    }
#endif

    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (draw) {
        nio_printf(&console, "%s", message);
        nio_DrawConsole(&console);
    }
#if USB_STORAGE_UART
    uart_printf("%s", message);
#endif
}
