# USB hook scanner

Build with your Ndless SDK:

```
make
```

Distribute `build/usb-hookscan.tns` and the instructions below. It is a standalone,
non-resident program; it does not depend on the USB driver or a connected USB disk.

## Instructions for testers

1. Use a CX II running one of these OS variants:
   - 6.2.0.333 NCAS, CAS or CX II-T
   - 6.4.0.74 NCAS, CAS or CX II-T
2. Reboot first. Run the scanner **before** launching usb-storage or other programs
   that patch the OS. Installed patches can hide the original signatures.
3. Launch `usb-hookscan.tns` and wait for the results screen.
4. Transfer the newly created `usb-hooks-<OS-ID>-<number>.tns` from Documents to
   your computer and send it back with the calculator model and OS version.
   This report is **plain text**, despite its transfer-friendly `.tns` extension;
   open it in a text editor, not on the calculator.
5. Missing or ambiguous results are useful too. Send the complete report rather
   than only the candidate address lines.

The program only reads mapped SDRAM and writes its own report file. It does not
install hooks, register a driver, change filesystem callbacks, mount USB media,
or execute functions found by scanning. It chooses a new report filename instead
of overwriting an existing report. UART output is disabled by default because a
serial write can block. To enable it explicitly, build with
`CPPFLAGS="-I.. -DUSB_HOOKSCAN_UART=1"`.

## What the report contains

- OS ID and profile name, scan bounds and signature reference build.
- Every signature as ARM instruction `value/mask` pairs, including reference
  addresses. Branch targets and PC-relative literal displacements are masked.
- Complete match counts and up to 16 candidate addresses per signature.
- Raw instruction words at each retained candidate.
- The parsedrive BL target and mount-table LDR relationship from `pc_findmount`.
- The live A: mount's drive number and all 29 filesystem callbacks, when the
  reference layout can be decoded without leaving allowed SDRAM.
- Cross-checks between the live callbacks and independently scanned signatures.
- A candidate `os_profiles.c` initializer with `.reviewed = 0`.

The default scan covers `[0x10300000, 0x10500000)`, containing all reference hook
instructions. Other builds may need a revised window if signatures are missing.
Progress appears every 256 KiB, followed by separate report-writing and closing
messages. The reader permits only `[0x10000000, 0x12000000)`, the first
32 MiB of CX II SDRAM. `os_profiles.c` controls the per-profile scan bounds.
Do not expand them into MMIO. Pattern matching can produce zero or multiple
results on another compiler/build; the scanner reports that instead of guessing.

## Extending the driver

See `../docs/adding-os-support.md`. A unique signature match is a candidate address,
not proof that the function signatures, structure layouts or patch prologue are
compatible. Pending profiles remain disabled in the resident driver until reviewed.

For a flat little-endian memory dump on a computer, the equivalent scanner is:

```
python tools/hook_scan.py scan sdram.bin --base 0x10000000 --os-id 49 --output hooks-49.txt
```

Run that command from the repository root. It expects a raw memory/code image at
the supplied base, not a compressed upgrade package or a container with headers.
It exits with status 2 when any required pattern is missing or ambiguous, while
still writing the report. It does not modify the input dump.
