CC = nspire-gcc
GENZEHN = genzehn
CPPFLAGS += -I..
CFLAGS = -std=gnu11 -marm -Os -Wall -Wextra -Werror -ffunction-sections -fdata-sections
LDFLAGS += -Wl,--gc-sections
OBJECTS = build/main.o build/hook_patterns.o build/hook_report.o build/os_profiles.o build/runtime.o

.PHONY: all clean
all: build/usb-hookscan.tns

build/main.o: main.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/%.o: ../%.c
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

build/usb-hookscan.elf: $(OBJECTS)
	$(CC) $^ $(LDFLAGS) -o $@

build/hook_patterns.o: ../hooks/patterns.inc

build/usb-hookscan.tns: build/usb-hookscan.elf
	$(GENZEHN) --input $< --output $@.zehn --name usb-hookscan
	make-prg $@.zehn $@
	rm $@.zehn

clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) build/usb-hookscan.elf build/usb-hookscan.tns

-include $(OBJECTS:.o=.d)
