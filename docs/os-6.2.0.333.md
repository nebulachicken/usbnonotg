# Target and hook evidence

Inspected in Ghidra: `dump-6.2.0-cxii-ncas.tns`, project `/ncas 6.2.0.333`,
ARM little-endian v5, 32-bit pointers.

SHA-256: `3b0bc1af94b845574de9e987fd6ac77bd7f4c6afa183cb2b1bbba54b4f4a017d`

This document is the reference ABI for OS ID 44. The driver selects a reviewed
entry from `os_profiles.c` and verifies all private-function signatures and
original A: callback addresses before changing OS memory. ID 44 is currently
the only enabled entry; five additional named profiles await review. See
`adding-os-support.md` for the extensible profile and scanner workflow.
The firmware hash identifies the analyzed dump; the driver does not hash the
entire running OS.

## Mount dispatch

`pc_findmount` at `0x10499c38` takes `const char *` in r0 and returns a Nucleus
mount object or NULL. It calls `pc_parsedrive` (`0x10499bcc`) and searches the
mount table. The first two ARM instructions are:

```
0x10499c38: e3500000  cmp r0, #0
0x10499c3c: e92d4038  stmdb sp!, {r3,r4,r5,lr}
```

Neither is PC-relative. The trampoline replays both and loads the original
continuation at `0x10499c40` without changing flags. The replacement is an
ordinary ARM C function with stack-local scratch space, rather than the SDK's
non-reentrant hook macros. The patch is installed with interrupts masked and
the instruction/data caches synchronized.

`NU_Open` (`0x10495f3c`, also the wrapper at `0x10494578`) selects a mount,
invokes `ops->open(drvno,path,flags,mode)` and allocates a global Nucleus file
descriptor using `pc_allocfile` (`0x1049b128`). Subsequent read/write/seek/close
operations recover that mount and the private descriptor. `pc_freefile`
(`0x1049b0d8`) clears the descriptor entry; it does not require the mount pointer
to be inside the OS mount array. `NU_Close` (`0x10495ee4`) frees its OS descriptor
even when the private close callback returns an error.

The overlay therefore returns a resident shadow of the A: mount with USB
callbacks for each matching USB namespace. It retains A:'s drive number and
device, and the original mount ID. The shadow's private file handles belong to
the USB adapter. Other paths call the original mount lookup trampoline.
No change to `fopen`, libc's FILE structure or application callers is needed.

The mount object is 0x28 bytes. Offsets:

| Offset | Field |
|---|---|
| 0x00 | filesystem callback table |
| 0x04 | filesystem registration |
| 0x08 | device |
| 0x0c | active flags |
| 0x10 | reference field |
| 0x14 | numeric drive letter |
| 0x16 | drive number |
| 0x1c | mount ID |
| 0x20 | two-byte drive name |
| 0x22 | six-byte device name |

In this dump, A:'s drive number is **1**, not 0. The driver obtains it from
the actual mount instead of assuming a value.

## Callback ABI

The 29-entry table is confirmed by `NU_Filesystem_Register` (`0x10499514`)
and the live Reliance table observed in the dump. Entries are enumerated in
`os_integration.c`; compile-time assertions enforce the ARM structure sizes.

Notable verified signatures:

```
open(uint16_t drv, const char *path, uint16_t flags, uint16_t mode)
read(int private_fd, void *buffer, unsigned count)
write(int private_fd, const void *buffer, unsigned count)
seek(int private_fd, int32_t offset, int whence)
get_attributes(uint8_t *attributes, const char *path)
set_attributes(const char *path, uint8_t attributes)
rename(const char *old, const char *next)
allocate_fsnode(uint16_t drv, const char *path, void **out)
deallocate_fsnode(uint16_t drv, void *node)
fsnode_to_string(uint16_t drv, const void *node, char *out)
wr_get_first(void *dstat, const char *pattern)
wr_get_next(void *dstat)
wr_done(void *dstat)
utime(void *dstat, uint16_t access_date, uint16_t access_time,
      uint16_t modify_date, uint16_t modify_time,
      uint16_t create_date, uint16_t create_time)
```

The previous driver's `get_attributes(drv,path,struct fat_stat *)` signature
was incompatible. It would interpret an output byte pointer as a drive number.

`__libc_convert_open_flags` (`0x1049247c`) confirms Nucleus access flags
0=read, 1=write, 2=read/write; append=0x008, create=0x100, truncate=0x200,
exclusive=0x400. `open` at `0x10491b30` confirms -3025 is EEXIST and -3017
is the name-length error. `relfs_seek` (`0x104ca634`) updates a logical file
position without extending the file. The adapter follows that behavior.

Error translation also follows the libc callers: -3020 is an invalid private
file descriptor, -3024 is a missing file, -3021 is access/write protection,
-3019 is descriptor exhaustion, and -3036 is an I/O/internal failure. Private
Nucleus drive-management errors such as -3001 must not escape as `open` results:
this libc's default error branch does not reliably convert them into failure.

## Working directories and rename boundaries

`NU_Set_Current_Dir` (`0x10495c00`) stores the allocated fsnode in the selected
drive's per-file-user working-directory array. `NU_Current_Dir` (`0x10495b74`)
finds a drive by letter and calls its `fsnode_to_string` callback.

Reliance fsnodes are path strings, confirmed by `relfs_allocate_fsnode`
(`0x104cc054`), `relfs_fsnode_to_string` (`0x104cc2e4`), and the working-directory
helper at `0x104ce5c4`. The helper copies the node string directly during
relative-path conversion.

The driver wraps A:'s allocate/deallocate/string callbacks. USB directory nodes
are owned allocations tracked in a list and represented as backslash paths.
Other nodes are delegated to the original Reliance callbacks. Retaining A:'s
drive number in the shadow mounts lets normal `chdir`, `getcwd` and relative
paths share the existing per-user working-directory machinery.

A:'s rename callback is also wrapped: renames across a USB/Reliance or
USB/USB-volume boundary are rejected. This prevents the original Reliance
rename callback from writing into a hidden mount directory when only the
destination is a USB path.

## Directory results and stat

`stat` (`0x10493508`) uses `NU_Get_First` even for exact paths and the mount root.
The adapter returns a single synthesized root result for that case and uses
FatFs search iterators only for wildcard paths. An empty mounted volume can
therefore still be statted as a directory.

The SDK's historical `DSTAT` fields do not describe this OS accurately.
Verified offsets from `relfs_wr_get_first` (`0x104cb3f8`), result conversion
(`0x104ce9f8`), `NU_Get_First` (`0x10495834`), `NU_Get_Next` (`0x104957d4`)
and `logfs_wr_get_next` (`0x1007eb60`):

| Offset | Field |
|---|---|
| 0x000 / 0x009 | short name / extension |
| 0x00d | long name, 256 bytes |
| 0x10d | FAT attributes |
| 0x110 / 0x112 | creation time / date |
| 0x114 | access date |
| 0x118 / 0x11a | modification time / date |
| 0x120 | 32-bit file size |
| 0x124 | OS-owned mount pointer |
| 0x128 | filesystem-owned search context |
| 0x12c | OS-owned mount ID |
| 0x130 | OS-owned drive number |

Result conversion leaves the OS-owned fields intact. `NU_Get_Next` compares
the saved mount ID against the mount object's ID, then calls that object's
callback. The USB context additionally validates the medium generation.

## Serialization

Verified Nucleus semaphore functions:

- create: `0x103a690c`
- obtain: `0x103a69dc`
- release: `0x103a6894`

The semaphore object is 0x28 bytes; create writes its last pointer at 0x24.
The driver uses a single recursive-by-current-task lock around mount/detach,
namespace callbacks and filesystem operations. This serializes FatFs tables,
SCSI BOT transactions and the controller's shared DMA storage. No filesystem
operation holds a task lock while waiting for the same lock recursively.

## Remaining hardware validation

Binary inspection establishes callback contracts, not successful execution.
The inherited low-level controller approach still needs validation on the real
CX II: physical DMA addressing, controller schedule handoff, data-toggle
continuity, reset recovery, cache coherency, disconnect during transfer, and OS
USB enumeration while filesystem access is active. The driver uses bounded
polling, checks transfer length/status and CSW signature/tag/residue/status,
and retains DMA storage permanently so a failed controller cannot DMA into
freed allocations. It does not claim full/low-speed or split-transaction support.
