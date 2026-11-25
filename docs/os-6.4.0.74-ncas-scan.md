# CX II 6.4.0.74 NCAS hook-scan review (OS ID 47)

Source: the on-device `usb-storage hook scan, format 1` report supplied for
OS ID 47. Scan range `[0x10300000, 0x10500000)`. Reference signature set:
CX II 6.2.0.333 NCAS, SHA-256
`3b0bc1af94b845574de9e987fd6ac77bd7f4c6afa183cb2b1bbba54b4f4a017d`.

The scanner explicitly warns that candidates are not ABI verification and that
no candidate code was executed. Matching compares ARM little-endian instructions
with branch destinations and literal-load displacements masked.

## Candidate addresses

| Hook | Candidate | Match count | Evidence |
|---|---:|---:|---|
| `pc_findmount` | `0x10499d00` | 1 | First two instructions match the reference prologue; parsedrive and mount-table relationships match. |
| `pc_parsedrive` | `0x10499c94` | 1 | Called by `pc_findmount` at `+0x18`; decoded branch target matches. |
| `SMC_Create_Semaphore` | `0x103a69bc` | 1 | Signature match. |
| `SMC_Obtain_Semaphore` | `0x103a6a8c` | 1 | Signature match. |
| `SMC_Release_Semaphore` | `0x103a6944` | 1 | Signature match. |
| `relfs_allocate_fsnode` | `0x104cc11c` | 1 | Live A: callback relation matches. |
| `relfs_deallocate_fsnode` | `0x104cc348` | 1 | Live A: callback relation matches. |
| `relfs_fsnode_to_string` | `0x104cc3ac` | 1 | Live A: callback relation matches. |
| `relfs_rename` | `0x104cae80` | 2 | The signature is ambiguous by itself; live A: `ops[17]` points to `0x104cae80`, selecting the second candidate. |

The reported live mount relationship is:

- mount table address: `0x11462fd0`
- A: mount: `0x11462fd0`, drive number `1`
- operation table: `0x11462a94`
- `ops[17]` (`OP_RENAME`): `0x104cae80`
- `ops[24]` (`OP_ALLOCNODE`): `0x104cc11c`
- `ops[25]` (`OP_DEALLOCNODE`): `0x104cc348`
- `ops[26]` (`OP_NODESTRING`): `0x104cc3ac`

The scanner reports that the parsedrive, mount-table, and filesystem-node/rename
relationships match the scan. The `pc_findmount` prologue is `cmp r0,#0` followed
by `stmdb sp!,{r3,r4,r5,lr}`, so its first two instructions do not contain a
PC-relative branch or literal load. This is useful trampoline evidence, not a
complete patch-safety review.

## What this report does not establish

The report itself says the relationships are decoded using the reference
instruction/structure layout. It does not independently verify the complete
OS ABI. In particular, the following are still unresolved for OS 6.4.0.74 NCAS:

1. The `nucleus_mount` layout and callback argument/return contracts used by
   `os_abi.h` and `os_integration.c`. The live callback pointers are strong
   cross-checks, but the scanner applies the reference layout when decoding them.
2. The `DSTAT` layout, especially the context at `0x128`, the preserved OS fields
   at `0x124`, `0x12c`, and `0x130`, and the fields populated by `ds_fill()`.
3. Nucleus semaphore structure size, object lifetime, and exact create/obtain/release
   calling conventions. Function signatures match; this is not a runtime ABI test.
4. BSD USB-stack structure offsets in `struct usb_os_usb_layout` (`device_t`,
   `usb_device`, pipe and endpoint descriptor offsets). These are not scanned by
   this report. They matter for the normal TI-OS USB-stack path; the passive
   forced-host path uses the driver's own host controller instead.
5. `TCC_Create_Task` address, entry signature, task-control-block layout, stack
   requirements, priority range, and scheduler behavior. The experimental
   background task must remain disabled until these are verified separately.
6. Physical hardware behavior: USB-controller ownership, cache/DMA coherency,
   idle-session persistence, reset/recovery, and file/directory operations.

For those reasons, `os_profiles.c` records the nine discovered hook candidates
for profile 47 but deliberately leaves `.reviewed = 0`,
`.abi = USB_OS_ABI_UNREVIEWED`, all USB layout fields zero, and all
`TCC_Create_Task` fields unset. `usb_os_select()` will reject the profile rather
than installing hooks against an unreviewed ABI.

## Next verification work

- Obtain function disassembly or a firmware dump for the OS 6.4.0.74 NCAS
  filesystem callbacks and compare their calling conventions and DSTAT accesses
  with `docs/os-6.2.0.333.md`.
- Verify the mount structure offsets from `pc_findmount` and its callers rather
  than relying only on the scanner's reference-layout decoder.
- Verify the semaphore object's size and operation semantics.
- If normal OTG mode is desired, separately inspect the 6.4 BSD USB structures.
- Keep the profile disabled until static ABI review is complete; then run all
  on-device tests before treating it as hardware-supported.
