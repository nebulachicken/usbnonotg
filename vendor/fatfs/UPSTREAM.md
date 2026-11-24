# FatFs provenance

Source: https://github.com/zephyrproject-rtos/fatfs
Commit: 4f60a12c58c3de4246857ad87c40239ecd93349b

Copied files:

- `ff.c`
- `include/ff.h`
- `include/diskio.h`
- `include/ffconf.h`
- `option/ffunicode.c`

The C implementation is unchanged from that commit. It includes the upstream
R0.16 patches and Zephyr's additional fixes, including zero-filling on file
extension. The project's `ffconf.h` enables read/write, directory searching,
attributes/modification timestamps, stack-based long-name buffers, UTF-8, three
volumes, multiple partitions, sector sizes through 4096 bytes, RTC timestamps,
creation-time reporting and file locking. Whole-operation serialization is
provided by the driver's recursive Nucleus semaphore, so FatFs's internal
reentrancy option is disabled. Formatting and exFAT are disabled.

`FS_FATFS_WINDOW_ALIGNMENT=4` supplies the standalone equivalent of the Zephyr
configuration value referenced by this version's header.

The author permits redistribution with the copyright, condition and disclaimer
retained. Those notices remain at the beginning of the vendored source files.
