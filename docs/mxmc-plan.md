# Plan: MXOS's own memory card and SIO2 drivers (mxmc, mxsio2)

Status (2026-11-16):
- Step 0 is done (mxio, the settings writer).
- Step 1 is done: `iop/sio/mxmc` with 16 open files. The kernel loads it in place of mcman.
  - Its import and export tables are the same as ps2sdk's mcman.irx.
  - Tested on a SCPH-70004 and a SCPH-90006:
    - `tests/ps2/k60_mclog.c hold`: 10 files and a `dopen` open at once;
    - `tests/ps2/k61_mcwrite.c`, in a test directory on the card in slot 1: 0 B to 300 KB, six
      files open and written in turns, a file written over shorter. 300 KB is written in
      about 3.4 s (~90 KB/s) and read in 0.65 s (~470 KB/s).
  - Like mcman, it has no `O_TRUNC` of its own: `O_CREAT` makes an existing file new, and
    without it a file keeps its length. Adding a real `O_TRUNC` is a possible change.
  - Formatting is not tested yet.
- Step 2 is done in a simpler form than planned: mxmc exports a library `mxmc`
  (`common/include/mxmc.h`) with `mxmc_card`. That is the card's own check, made under mxmc's
  lock without taking a file, plus its size and free space. mxio calls it every second in
  place of `dopen`; it is a poll, not a callback, because a memory card gives no interrupt when
  it comes or goes, so mxmc would poll too. Size and free space go into the device list
  (`mx_dev_list`), which the kernel already passed on, so no `devctl` and no kernel change were
  needed. mxmc counts the free space again only after a page was written or erased.
  Tested on a SCPH-70004 and a SCPH-90006: k61 sees the free space go down by what it wrote
  and come back.

## Why

The kernel loads ps2sdk's prebuilt `sio2man`, `mcman`, `mcserv` and `padman`
(`apps/mxos/ee/main.c`, `module_names`). `mcman` has room for only 3 open files
(`MAX_FDHANDLES`), shared by both slots. The VM keeps every running program's ELF
open, because its pages are read from it (`ee/src/vm/image.c`). With the service
(`mc0:/MXOS/MXLINK.ELF`) and the shell (`mc0:/MXOS/MXSHELL.ELF`) running, only one
handle is left. Every other user of the card fights over that one, and the loser
gets `-EMFILE` (-24). Measured on the PS2 with `tests/ps2/k60_mclog.c hold`.

What that causes today:

- **False "Memory card removed" / "connected" notifications.** mxio's watcher
  checks each card once a second with `dopen("mc0:/")`, and counts any failure,
  -24 included, as no card.
- **Settings that cannot be written.** The kernel's writer (`apps/mxos/ee/settings.c`)
  fails, logs it without the error code, and tries again only 30 s later.
- **A game started from mc0: would leave no handle at all.**

The card driver is also the one part of the card's path we cannot change:
- it has no way to tell anyone that a card came or went;
- it gives no other way to get the card's free space and type than the MCSERV RPC;
- it shares the SIO2 with `padman` and with `mx4sio_bd`, which drives the SIO2
  registers itself. That is why the kernel does not load MX4SIO (kernel.md,
  version 26).

## What uses the SIO2 and the card now

| Interface | User | Notes |
|---|---|---|
| SIO2 hardware | `sio2man` (owner); `padman`, `mcman` through its exports (`sio2man`, `xsio2man`, `rsio2man` names); `mx4sio_bd` directly, without sio2man | sio2man serializes with one semaphore: `sio2_*_transfer_init` takes it, `sio2_transfer_reset` gives it back |
| Card authentication | `mcman` → `secrman` (`SecrAuthCard`, `SecrSetMcCommandHandler`, `SecrSetMcDevIDHandler`) | The ROM's SECRMAN, resident after an IOP reset. mxboot's installer uses its own SECRMAN image; not part of this plan |
| ioman/iomanX device `mc` | mxio (every EE file call: `mx_fio_*`, settings, the swap file on a card, the kernel's module and ELF loading, the VM pager); ps2link's FILEIO for programs on the old path | Keep this device: everything above it stays as it is |
| MCSERV RPC 0x80000400 | only `apps/mxboot/install/install.c` (format) | MCSERV binds to mcman's exports |
| Card presence | mxio's `present()` (`dopen` every second per slot) | Should come from the driver itself |
| Pads (SIO2 ports) | `padman`; the kernel's `ee/src/base/pad.c` over its RPC (0x8000010F), with the state in EE memory | The USB pads (mxpad) already write their state by SIF DMA into `.padarea` |
| Boot | mxboot and the kernel's recovery `rom_card()` use the ROM's SIO2MAN/MCMAN/MCSERV | Stays: the fallback when a card module is broken |

## Design

**mxmc.irx** (`libs/maxsdk/iop/sio/mxmc`) replaces `mcman`:
- It starts as a fork of ps2sdk's `iop/memorycard/mcman`, keeping its license file
  (as `iop/dev9` does), and is built with clang/ld.lld.
- It is a fork, not a rewrite, because the card's file system is where a bug
  costs people their saves. mcman's handling is tried and tested:
  - ECC;
  - bad block replacement;
  - the backup block that makes a write survive a power cut;
  - PS1 cards.
- It keeps mcman's export table, so the prebuilt MCSERV still binds to it and
  the installer still formats.
- Changes:
  - Open files: 16, not 3. A handle is small; the cache stays shared.
  - A presence callback, as `bdm_RegisterCallback`. The driver already sees a
    card change in its own checks (the probe command, a new card's flag). Card
    events go to mxio through the callback, and mxio's `dopen` poll goes.
  - Free space, card type and format as `devctl` on `mc`, so programs and the
    shell need no MCSERV.
  - Errors that say what happened. `-EMFILE` must never be read as "no card".
- Nothing else changes at first: same cache, same speed. Speed is measured in
  step 1 and is its own later step.

**mxsio2.irx** (`libs/maxsdk/iop/sio/mxsio2`) replaces `sio2man`:
- Same exports, so the prebuilt `padman` keeps working.
- One lock for all users of the SIO2, with a published function to take it.
- `mx4sio_bd` gets a small fork that takes that lock around its transfers. MX4SIO
  in slot 2 and a memory card in slot 1 then work at the same time.

**The pads, later:**
- Read the SIO2 pads in mxsio2 (or a module of its own on it) each vblank.
- Write their state by SIF DMA into `.padarea`, as mxpad does for USB.
- `padman` and the EE side of its RPC then go.
- Multitap goes in the same step.

## Steps

0. **Now, without new modules** (fixes the symptoms):
   - mxio `present()`: treat only `-ENXIO` (and an unformatted card's error) as
     no card; on any other error keep the slot's state.
   - Settings writer: put the error code in the log; after -24 or a busy card,
     try again in 2 s, not 30.
   - Both cards need a new kernel and `mxio.irx`.
1. **mxmc, a straight fork:**
   - It builds with clang, gives 16 handles, and the kernel loads it in place of
     `mcman`.
   - Tests on PCSX2 and the PS2, on mc0 only, in a test directory:
     - k60 `hold` opens 16 files;
     - write/read/rename/delete/getstat compared byte for byte;
     - many files open in both slots;
     - a card pulled and put back by hand;
     - the installer's format, on a spare card.
   - Measure read and write speed against the ROM's MCMAN and ps2sdk's.
2. **mxmc's card check and free space** (done: see the status at the top):
   - mxio checks the cards through mxmc, without a file.
   - Size and free space for the shell, in the device list.
   - Document both in mx-os.md and kernel.md.
3. **mxsio2 and the mx4sio_bd fork:**
   - The SIO2 lock, so the kernel can load MX4SIO again (a setting already
     exists: `storage.mx4sio`).
   - Test with a pad, the card and MX4SIO together.
4. **SIO2 pads in our own module:**
   - `padman` and its RPC go; pad state by SIF DMA as for USB; multitap.
   - Kernel version bump.
5. **Clean-up:**
   - The installer formats through `devctl`, and MCSERV is no longer loaded.
   - Speed work in mxmc (bigger transfers, the cache), if step 1's numbers call
     for it.

Steps 0 and 1 each need the kernel and IOP modules installed on both cards (ask
first). Steps 2 to 4 change how programs run, so mx-os.md and kernel.md change in
the same commit.

## Open points

- A spare memory card for the write and format tests, so no one's saves are on it.
- How mcman reports a card change without a handle (`McDetectCard` / the
  probe), and how often it may probe without slowing the pads.
- Whether the prebuilt `padman` binds to mxsio2's exports unchanged (it imports
  both `sio2man` and `xsio2man`).
- PCSX2 emulates the SIO2 at the register level, so steps 1 to 4 can be tried
  there first; card authentication too.
