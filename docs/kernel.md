# mx-kernel and its libraries

The services a program uses have one interface, the `<mx/...>` headers. A program chooses
its implementation when it links (`MX_MEMORY`, `MX_KERNEL`, `mk/maxsdk.mk`):

| | `libmx.a` | `libmxk.a` |
|---|---|---|
| Link with | `MX_MEMORY=phys` | `MX_MEMORY=vm`, or `MX_KERNEL=1` for all in core |
| Runs | on a bare PS2: ps2link, PCSX2, and under mxlink | only under mx-kernel; stops with a message without one |
| TLB | the BIOS's | the kernel's |
| Memory | 31 MiB, physical | a 128 MiB virtual window, and core memory |
| Files, VM, heaps, exceptions | its own; no VM at all | the kernel's: `libmxk` has only stubs |
| Pad, sound | its own SIF client | its own, over the kernel's SIF |

A program's source is the same for both; only the room differs. What only makes sense
with virtual memory exists in `libmx` too (`ee/src/vm/novm.c`), doing nothing, so programs and
libgl need no `#ifdef`. Graphics (GS, VIF, DMA) are not a service: they are direct code in
both.

A third library, `libmxkern.a`, is `libmx` with the VM: for the kernel, and for the tests
of the VM's inside, which run with the VM in the program. Programs do not link it.

The kernel is `tools/mxlink`, resident at 0x88000-0xFF800. `libmxk` calls it through
`struct mxk_services` (`<mx/kernel.h>`), a table the loader passes. Only `libmxk` sees how
a call reaches the kernel, so that can change without changing programs. Kernel code runs
in the caller's thread; the VM's writer and pager are threads of the kernel. Functions are
only added at the end of the table, so older programs keep working; `libmxk` needs a table
of at least its own version.

## Service groups

| Group | Headers | State |
|---|---|---|
| Program | `<mx/loader.h>`, `mx_exit` | the kernel's (version 1) |
| SIF | `<mx/sif.h>` | the kernel's (version 2) |
| Exceptions | `<mx/debug.h>` (`mx_exception_init`, `mx_crash_last`) | the kernel's (version 3) |
| Files | `<mx/sif.h>` (`mx_fio_*`) | the kernel's (version 4), through mxio if the IOP has it, else FILEIO |
| Memory | `<mx/vm.h>`, `<mx/mem.h>` | the kernel's (version 4) |
| IOP | `<mx/iop.h>`, `<mx/sif.h>` | over the kernel's SIF |
| Pad, sound | `<mx/pad.h>`, `<mx/snd.h>` | program's own, over the kernel's SIF |
| Threads | `<mx/thread.h>`, `<mx/sched.h>` | program's own |
| Network | to be defined | |

The program's first call gives the kernel its core memory after `_end` and its VM part;
the heaps and the program break work in those. The kernel's VM state is cleared by the
restart after each program.

## The restart

Each program ends in `ExecPS2`, which starts the kernel again but does not load it: `.bss`
is cleared by the start-up, and `.data` is restored from a copy by `mxlink_start`
(`ee/hook.S`), so all start values are as linked. What must survive goes in the section
`.kept`. Without this a semaphore id in `.data` points at a semaphore of the next program.

## PCSX2

`make -C tools/mxlink emu` builds the kernel for PCSX2 (`build/tools/mxlink-emu/mxlink.elf`,
`MX_LOADER` in `mk/maxsdk.mk`): no network, no IOP modules, no commands. It starts the
program in `host:mxload.txt` (path and arguments, in words) from the directory of the
ELF. Run it with `PCSX2_EE=interp`.

## Crashes

The kernel's exception vector stays installed while a program runs. It reports what the
VM cannot handle: registers, stack and the `trace:` line go to the host, on the kernel's
stack. A crash inside a file call does not block the report: the file lock is given up.
The program ends with status 255 and the kernel restarts.

The report survives the restart in the kernel's `.data`. `mx_crash_last` gives it to
the next program (the shell shows it, `libmxk` in core), and the text screen shows it. It
belongs to the program that ended last: the next return replaces it, and a reset drops it.

- The report never reads VM: a page fault there might not end. Trace words in code in VM
  are not checked for a call before them, so one can be stale; `tools/symbolize.py` shows
  what they are.
- A `libmxk` program's unflushed `mx_debug_hold` output, and a line it has not ended, are
  lost in a crash.
- A `libmx` program installs its own handlers and reports itself; the kernel only sees
  status 255.
- A program without maxsdk (ps2sdk) is reported by the kernel on the serial port only: it
  has its own SIF. The report is kept, and status 255 is set.

## Files: mxio

`iop/fs/mxio` is the file service in place of `rom0:FILEIO`, loaded by mxlink when it is on the
card (`common/include/mxio.h`). `mx_fio_open` uses it if it is there: its descriptors start at 64,
each call names its position, so a seek costs no call. Directories, remove, mkdir and appending
stay with FILEIO.

`host:` takes the fast path when a `ps2client` has said HELLO: data goes past lwIP (`iop/tcpip/mxnet`,
`common/include/mxfast.h`), straight between SMAP and EE memory. Any other path, and `host:`
without HELLO, takes the driver through ioman. In 32 KiB, measured on a PS2: 7.6 MiB/s reading and
4.6 MiB/s writing on `host:`, against 2.1 and 1.6 through FILEIO. USB and the memory card are limited
by the device or its driver.
