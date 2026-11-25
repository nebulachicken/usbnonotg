#!/bin/bash
set -eu
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/include -I. -Ivendor/fatfs tests/test_driver.c namespace.c fat.c usb_diag.c \
    vendor/fatfs/ff.c vendor/fatfs/ffunicode.c -o build/test-driver
./build/test-driver
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -Itests/include -I. tests/test_host.c usb_host.c usb_storage_scsi.c -o build/test-host
./build/test-host
cc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -I. tests/test_hook_patterns.c hook_patterns.c os_profiles.c -o build/test-hook-patterns
./build/test-hook-patterns
python3 -m unittest discover -s tests -p 'test_hook_scan.py'
