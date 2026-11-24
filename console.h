#ifndef CONSOLE_H
#define CONSOLE_H

int usb_console_begin(void);
void usb_console_end(void);
void usb_log(const char *format, ...) __attribute__((format(printf, 1, 2)));

#define log(...) usb_log(__VA_ARGS__)

#endif
