CC = nspire-gcc
GENZEHN = genzehn
CFLAGS = -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -marm -Os -ffunction-sections -fdata-sections
CPPFLAGS += -I. -Ivendor/fatfs
LDFLAGS += -Wl,--gc-sections
SOURCES = usb.c bulk.c otg_host.c usb_host.c usb_diag.c usb_storage_scsi.c fat.c namespace.c os_integration.c \
          os_profiles.c os_runtime.c hook_patterns.c runtime.c console.c \
          vendor/fatfs/ff.c vendor/fatfs/ffunicode.c
OBJECTS = $(SOURCES:%.c=build/%.o)

.PHONY: all clean distclean test hookscan usbtest dist patterns-check
all: build/usb-storage.tns
build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
build/usb-storage.elf: $(OBJECTS)
	$(CC) $^ $(LDFLAGS) -o $@
build/usb-storage.tns: build/usb-storage.elf
	$(GENZEHN) --input $< --output $@.zehn --name usb-storage
	make-prg $@.zehn $@
	rm $@.zehn
test:
	bash tools/test.sh
hookscan:
	$(MAKE) -C hookscan_program
usbtest:
	$(MAKE) -C usbtest_program
# Everything that gets installed on the calculator, collected in dist/
dist: all hookscan usbtest
	mkdir -p dist
	cp build/usb-storage.tns dist/
	cp hookscan_program/build/usb-hookscan.tns dist/
	cp usbtest_program/usbtestprogram.tns dist/
patterns-check:
	python3 tools/hook_scan.py generate --check
build/hook_patterns.o: hooks/patterns.inc
clean:
	rm -f $(OBJECTS) $(OBJECTS:.o=.d) build/usb-storage.elf build/usb-storage.tns
	$(MAKE) -C hookscan_program clean
	$(MAKE) -C usbtest_program clean
distclean: clean
	rm -rf dist
-include $(OBJECTS:.o=.d)
