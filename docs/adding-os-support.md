# Adding an OS profile

The driver now separates three things that were previously mixed together:

1. **OS identity and verified addresses** in `os_profiles.c`.
2. **Nucleus callback/mount ABI** in `os_abi.h` and its adapter in `os_integration.c`.
3. **USB stack structure offsets** in each profile's `usb` member, consumed by
   `usb.c` and `bulk.c`.

Hardware MMIO registers remain in the CX II controller implementation. Do not
change them merely because an OS function moved.

## Profile status

The OS IDs below match the Ndless source's `ut_os_version_index` mapping in
`ndless/src/resources/utils.c` at SDK checkout
`9484d8da7c7a4dde9766138c2e42e1d1e3acfcd4`.

| OS ID | Profile | Driver status |
|---|---|---|
| 44 | CX II 6.2.0.333 NCAS | Addresses/ABI reviewed against supplied dump; hardware testing pending |
| 45 | CX II-T 6.2.0.333 | Pending scanner report and ABI review |
| 46 | CX II 6.2.0.333 CAS | Enabled with captured hooks and maintainer-confirmed shared core layouts; hardware testing pending |
| 47 | CX II 6.4.0.74 NCAS | Pending scanner report and ABI review |
| 48 | CX II-T 6.4.0.74 | Pending scanner report and ABI review |
| 49 | CX II 6.4.0.74 CAS | Pending scanner report and ABI review |

All six have named profile entries and can run the standalone scanner. Only ID
44 and 46 are currently enabled for driver installation. The other addresses and
USB offsets remain unset until reviewed.
No uniform address delta is assumed.

The CAS report is preserved in `tests/fixtures/usb-hooks-46-01.txt`. Eight
signatures have one match each. The rename signature also matches another
function at `0x104cbc5c`; live A: slot 17 identifies `0x104cbdb8` as rename.
All four reported node/rename callbacks and the parsedrive call agree with the
scan. The maintainer confirmed that CAS and NCAS share core structure layouts;
ID 46 therefore uses the NCAS ABI and USB offsets with these captured addresses.
This does not constitute a completed USB hardware test. Runtime signature and
live callback checks remain required before installing hooks.

## Collect and review hooks

Give testers the standalone scanner and `hookscan_program/README.md`. Build it
with `make hookscan` from the root, or `make` inside `hookscan_program/`. It saves
the pattern definitions and candidate instructions alongside the discovered
addresses. Send the whole report back, including missing/ambiguous results.

Nine hook/function addresses are required:

| Profile hook | Purpose |
|---|---|
| `USB_HOOK_FINDMOUNT` | Route USB namespace paths to shadow mounts |
| `USB_HOOK_PARSEDRIVE` | Respect explicit/current A: drive selection |
| `USB_HOOK_SEMAPHORE_CREATE` | Initialize serialization |
| `USB_HOOK_SEMAPHORE_OBTAIN` | Enter serialization |
| `USB_HOOK_SEMAPHORE_RELEASE` | Leave serialization |
| `USB_HOOK_ALLOCATE_FSNODE` | Validate the original A: working-directory callback |
| `USB_HOOK_DEALLOCATE_FSNODE` | Validate/delegate original node destruction |
| `USB_HOOK_FSNODE_TO_STRING` | Validate/delegate original cwd conversion |
| `USB_HOOK_RENAME` | Validate/delegate original Reliance rename |

For each pending OS:

1. Check unique matches and inspect their disassembly. The scanner never calls
   candidates. If a signature misses or collides, inspect the target binary
   before changing the pattern; do not select the first hit.
2. Check that `pc_findmount` actually calls the discovered parsedrive and that its
   table leads to the active A: mount. The report decodes these relationships
   using the reference instruction offsets and labels that assumption.
3. Check that the live A: callbacks agree with the scanned allocate, deallocate,
   string conversion and rename functions.
4. Review the contracts recorded in `docs/os-6.2.0.333.md`: callback order and
   signatures, mount layout, per-user cwd representation, DSTAT offsets,
   descriptor allocation/freeing, errors and semaphore object size/semantics.
5. Confirm the first two findmount instructions can be replayed at a different
   address. The current signature starts with a CMP and register push. Relative
   branches/literal loads cannot simply be copied into the trampoline.
6. Verify the BSD USB device/pipe offsets in the target OS. Fill every field in
   the profile's `usb` member; a filesystem-only report does not prove them.
7. Copy the reviewed addresses into that OS's existing entry. Select
   `USB_OS_ABI_NUCLEUS_CX2_V1` only if the ABI is the same, then set `.reviewed = 1`.
   A different ABI needs its own adapter, not just a different address table.
8. Run the on-device stdio test and USB hardware cases, including relative paths,
   normal Reliance access, multiple devices/partitions, unplug/replug and errors.

Driver startup validates every configured function against its signature and
logs the address and match status to the startup console (and optionally UART).
It then checks the live A: callback
table before patching. It does not auto-enable scanner candidates or overwrite
profiles from report files.

## Signature source and logs

`hooks/patterns.json` is the authoritative signature manifest. Each word stores:

- `reference`: instruction from the analyzed 6.2.0.333 NCAS binary;
- `value`: expected fixed bits;
- `mask`: bits compared during a match.

The scanner uses `(instruction & mask) == value`. ARM B/BL destination fields
and literal-LDR displacement/U bits are masked; register/opcode bits remain.
These masks allow relocation but do not promise compatibility with a recompiled
function. The signatures are complete multi-instruction sequences rather than
two-instruction prologues.

Generate/check the C table after editing the manifest:

```
python tools/hook_scan.py generate
python tools/hook_scan.py generate --check
```

Commit/distribute both the manifest and generated `hooks/patterns.inc`. The driver
and on-device scanner link the same C table. The offline Python tool reads the
same manifest, so the report format and definitions can be inspected without
an ARM build.

Interpreted regression tests can be run without building the driver:

```
python -m unittest discover -s tests -p test_hook_scan.py -v
```

Those tests cover the captured reference instruction sequences, relocation masks,
missing/ambiguous candidates, alignment, report generation, ARM branch/literal
decoding and generated-table consistency. They do not establish uniqueness in
an entire OS image. C scanner bounds/block-overlap tests are also supplied in
`tests/test_hook_patterns.c` and included in `make test` for when you build tests.

## Source style

`.clang-format` specifies four-space indentation, 100-column lines, expanded
control-flow braces and separate function definitions. Maintain ordinary functions
for the filesystem wrappers; avoid reintroducing function-generating macros.
Vendored FatFs source and generated signature data are intentionally not reformatted.
