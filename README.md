# USB storage overlay

Resident USB mass-storage driver with named profiles for **CX II 6.2.0.333 and
6.4.0.74, NCAS/CAS/T**. Installation is enabled for **6.2.0.333 NCAS and CAS**.
NCAS uses the supplied binary's addresses; CAS uses the captured on-device hook
report and the maintainer-confirmed shared core layouts. USB hardware testing
is pending. The other four profiles remain disabled pending hook discovery.

The standalone [hook scanner](hookscan_program/README.md) finds and logs candidate
addresses on all six variants. See [adding OS support](docs/adding-os-support.md)
for the profile table, signature format and review workflow.

Launching the driver displays a startup console with hook checks, filesystem
initialization and USB registration results. Press a key after completion to
return to Documents; on success the driver remains resident. The console is
closed before returning, so later USB events do not draw over the OS screen.
UART logging is disabled by default to avoid blocking serial writes. Enable it
with `-DUSB_STORAGE_UART=1` in `CPPFLAGS` when using a working serial connection.

Up to three FAT volumes appear at:

```
A:/documents/usbA/
A:/documents/usbB/
A:/documents/usbC/
```

The first available slot is assigned to each attached FAT volume, including
multiple supported primary partitions on one device. These are directory
overlays in the A: namespace; no additional Nucleus drive letters are needed.
Other paths continue to use the existing Reliance filesystem.

The driver implements ordinary file creation, reading, writing, append, seek,
flush, shrinking truncation, rename, removal, directory creation/enumeration,
attributes, modification timestamps and working directories. Applications use
their normal `fopen`, `fread`, `fwrite`, `stat`, `opendir`, `chdir`, etc.
For example:

```c
FILE *f = fopen("A:/documents/usbA/example.txt", "w");
if (f) {
    fputs("Hello from USB\n", f);
    fclose(f);
}
```

## Forced host mode (passive adapter, no OTG adapter)

Ported from `otgweb.c`. When the driver is launched and the ID pin is not
grounded, it offers to force USB host mode on the top (mini-USB) port:

1. GPIO0 bit 2 is driven low to switch the calculator's own 5 V on.
2. HNP is armed (pull-up variants A, B, C are tried in turn) until the
   controller flips to host role.
3. The host controller is started, the port is reset, and the drive is
   enumerated by the driver itself (no OS USB stack involved).
4. The drive is checked for readiness, the filesystem hooks are installed and
   the FAT volumes are mounted under `A:/documents/usbA`..`usbC`.

If anything fails before the hooks are installed, the hardware is restored,
a message is shown, and nothing stays resident, so you can simply retry.
The bring-up log is written to `usb-storage_log.txt.tns` next to the program.
**Please send that file with any bug report: this path has not been run on a
calculator yet.**

Choosing "Skip", or an OTG adapter (ID grounded), uses the previous behaviour:
TI-OS is the host, the driver registers with its USB stack, and hot-plug works.

Differences from the OS-stack path:

- **One session per boot.** The drive must be plugged in before launching, and
  there is no hot-plug. If it is unplugged, or the session is lost, the volumes
  become unavailable and 5 V is switched off at the next access. Mounting again
  needs a reboot, because the filesystem hooks cannot be installed twice.
- **5 V stays on while mounted** (the calculator's battery feeds it). Use a
  self-powered drive. Do not connect a computer, charger or powered hub: the
  driver refuses to start if it sees VBUS, and an external supply on VBUS must
  never be wired to the calculator's pin (see the Y-cable discussion).
- Full-speed and high-speed devices are accepted (the OS-stack path is
  high-speed only). Low-speed devices are rejected.
- Bulk transfers are made in 4 KiB chunks, and the drive may take up to 10 s to
  report ready (spinning disks).
- TI-OS keeps running while the driver polls the controller (IRQ mode 2, as in
  `otgweb`); between transfers TI-OS may re-gate or re-mask the USB block, so
  every transfer re-checks the session first and gives up cleanly if it is gone.
  How TI-OS behaves toward an idle host session is the largest unknown.

## Source layout

- `os_integration.c`: Nucleus mount selection, callback ABI, DSTAT conversion,
  Reliance working-directory interoperability and serialization.
- `os_profiles.c`: per-OS addresses, ABI status, scan ranges and USB structure offsets.
- `os_runtime.c`: profile selection, signature checks and typed private OS calls.
- `os_abi.h`: reviewed Nucleus callback/mount layout.
- `hooks/patterns.json`: shared, logged ARM signature definitions.
- `hookscan_program/`: standalone on-device discovery/report program.
- `tools/hook_scan.py`: offline dump scanner and C signature-table generator.
- `namespace.c`: bounded normalization and exact mount-prefix matching.
- `fat.c`: FatFs adapter, volume/file lifetime and USB disk interface.
- `usb_storage_scsi.c`: checked Bulk-Only Transport and SCSI block commands.
- `bulk.c`: FOTG210 asynchronous-schedule transfer implementation.
- `vendor/fatfs/`: vendored filesystem implementation and configuration.
- `docs/os-6.2.0.333.md`: reverse-engineering evidence and hook contracts.
- `tests/test_driver.c`: host regression tests using in-memory disk images.
- `usbtest_program/usbtest.c`: on-device tests using ordinary libc calls.

## Build and validation

Use your Ndless SDK and ARM toolchain with `make`. Sources are explicitly listed
so the test programs, downloaded tools and SDK sources are excluded from the
resident driver. The main output target is `build/usb-storage.tns`.

`make hookscan` builds the separate distributable scanner at
`hookscan_program/build/usb-hookscan.tns`. Run it before installing the driver;
it saves a uniquely named plain-text `.tns` report in Documents. Scanner UART
output is opt-in. Reports include the actual search patterns, match counts, raw
candidate instructions, live callback checks and a profile initializer.

`make test` runs the host tests with a native C compiler and AddressSanitizer /
UndefinedBehaviorSanitizer. These exercise namespace boundaries, FAT12 disk I/O,
file extension/gap clearing, append, UTF-8 long names, directories, MBR mounting,
multiple devices, failed I/O and stale handles after reconnect. They do not
exercise the OS hooks or controller hardware. The on-device test program also
checks the full stdio and working-directory route. It creates its own directory
and refuses to overwrite an existing one.

Building and packaging are left to the owner as requested. The seven interpreted
Python scanner/signature tests pass, and the generated C pattern table has been
checked against its manifest. **This source revision and the on-device scanner
have not been built or validated on a calculator.** The supplied C tests have
not been run. Earlier intermediate build results do not validate this revision.

## Operating boundaries

- FAT12/16/32, raw FAT media or supported primary MBR partitions, logical sectors
  of 512/1024/2048/4096 bytes. No exFAT, GPT or extended-partition traversal.
- LUN 0 only. This transfer implementation accepts high-speed USB devices;
  full/low-speed devices and hub split transactions are unsupported.
- File positions and file sizes are limited to signed 32-bit values to match OS
  clients. Seeking beyond EOF does not grow a file; a subsequent write fills the
  gap with zeros. Truncation grows no files.
- Up to 16 private file handles; the OS itself has a smaller global descriptor
  table. Long names use UTF-8 within the OS's 255-byte path limit.
- Formatting, USB-specific drive-letter/free-space queries, and changing access
  or creation timestamps through `NU_Utime` are not implemented. Existing FAT
  creation timestamps are reported; modification timestamps are supported.
- The three real mount directories remain visible when no device is present.
  Opening their contents then fails instead of accessing hidden NAND files.
  Existing content in those directories is hidden by the resident overlay.
- Unplug invalidates all handles and searches belonging to that device. Old file
  IDs remain reserved until close, so they cannot address a replacement device.
  Close/flush errors are reported; a failed close takes the medium offline.
- Close files before unplugging. No unload or in-session hook removal is
  implemented; reboot clears the resident driver and hooks. A second launch is
  rejected by the original-instruction check.
- Controller schedule ownership is held with CPU interrupts masked for each
  bounded bulk transfer. Normal scheduling resumes between transfers. Timeout
  and fatal-controller paths need particular attention during hardware testing.

The original supplied sources are preserved in `original-driver.zip`.
`.toolchain/` contains ignored SDK/reference downloads from development; the
driver's build only needs the vendored FatFs files and your normal SDK.

## Forced-host source files

- `otg_host.c/.h`: bring-up, session guard, polled control and bulk transfers.
- `usb_host.c/.h`: enumeration, Bulk-Only interface search, host transport.
- `tests/test_host.c`: host tests with a fake Bulk-Only drive (descriptor
  parsing, toggle accounting, chunking, the real SCSI layer, session loss).

## Third-party source

FatFs R0.16 is vendored from the
[Zephyr FatFs repository](https://github.com/zephyrproject-rtos/fatfs), commit
`4f60a12c58c3de4246857ad87c40239ecd93349b`. Its copyright/license notices are
retained in the files. See `vendor/fatfs/UPSTREAM.md` for provenance and local
configuration changes.

## Experimental background USB maintenance

The forced-host source contains an optional Nucleus worker (`usb_background.c`).
If enabled for a verified OS profile, it periodically checks the existing USB
host session and reapplies the controller's USB clock/interrupt guard while the
calculator is idle. It is serialized with FAT/SCSI operations using the same
filesystem lock. It does not perform transfers, re-enumerate a disconnected
device, or reapply VBUS power after a session has been lost.

**This worker is disabled for the supplied CX II OS 6.2.0.333 profiles.**
`nl_set_resident()` only preserves the executable's memory after exit; it does
not schedule code. Hackspire's published Ndless syscall list does not expose
Nucleus task creation, and the community-documented `TCC_Create_Task` entry is
an unstable internal API. The source deliberately leaves its function address
and code signature unset rather than guessing. Enabling it requires verifying
the exact OS image's function entry, ARM calling convention, task-control-block
size, task priority range, and stack requirements.

The driver therefore continues to rely on its existing on-demand host-session
checks until those details are independently confirmed.
