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
only added at the end of the table, so older programs keep working. `libmxk` needs a table
of at least `MX_KERNEL_MIN` (5); what came later it uses only when the kernel's table has it,
else its own code (the pad: version 6), so a new program still runs on an older kernel.

## Service groups

| Group | Headers | State |
|---|---|---|
| Program | `<mx/loader.h>`, `mx_exit` | the kernel's (version 1) |
| SIF | `<mx/sif.h>` | the kernel's (version 2) |
| Exceptions | `<mx/debug.h>` (`mx_exception_init`, `mx_crash_last`) | the kernel's (version 3) |
| Files | `<mx/sif.h>` (`mx_fio_*`) | the kernel's (version 4), through mxio if the IOP has it, else FILEIO |
| Memory | `<mx/vm.h>`, `<mx/mem.h>` | the kernel's (version 4) |
| IOP | `<mx/iop.h>`, `<mx/sif.h>` | over the kernel's SIF; `mx_iop_reboot_image` (an IOPRP image in EE memory) is libmx's only: -1 under the kernel, which resets the IOP by a path |
| Pad | `<mx/pad.h>` | the kernel's (version 6, `ee/src/base/pad.c`'s reading in the kernel's memory): it opens the ports when a program asks, applies the pad's settings (ports swapped, Cross and Circle swapped: `tools/mxlink/ee/pad.c`) and closes them when the program ends; on a kernel of version 5 the program's own, over the kernel's SIF |
| Sound | `<mx/snd.h>` | program's own, over the kernel's SIF |
| Threads | `<mx/thread.h>`, `<mx/sched.h>` | program's own; `mx_sleep_us` takes EE timer 0, which `mx_exit` stops |
| Network | `<mx/net.h>` | a service on the IOP, `iop/tcpip/mxsock`, loaded by mxlink when it is on the card, on lwIP over maxsdk's drivers (mxdev9, mxeth, mxnet) or, without them, ps2sdk's; closed when a program ends. `mx_net_wait` sleeps at one of its four wait services (at most 100 ms a call); with an older mxsock it asks every millisecond |

The program's first call gives the kernel its core memory after `_end` and its VM part;
the heaps and the program break work in those. The kernel's VM state is cleared by the
restart after each program.

## The restart

Each program ends in `ExecPS2`, which starts the kernel again but does not load it: `.bss`
is cleared by the start-up, and `.data` is restored from a copy by `mxlink_start`
(`ee/hook.S`), so all start values are as linked. What must survive goes in the section
`.kept`. Without this a semaphore id in `.data` points at a semaphore of the next program.

`mx_exit` ends the program's other threads first (those of `mx_thread_start`): it goes on to give
the BIOS its TLB back and to block in file calls, and a thread that ran then would touch VM with
its entries gone. It waits a moment for the file lock first (version 5), so that none of them ends
in a file call with the lock taken, which the rest of `mx_exit` would wait for. The VM's pager and
writer are not ended.

When a program comes back, whichever way it ended, mxlink prints the test end marker with its exit
status, before it says it is ready. A run on the host then ends, also when the player left the
program with the pad. The console (the tty) reaches the ps2client that serves the fast path
without loss, the marker included, and a later ps2client gets none of an earlier run's rest
(`iop/tcpip/mxfast`, `docs/iop-drivers.md`); other clients get it by broadcast, as from udptty.
It stays silent when the program started another with `mx_loader_launch`: the
run ends with the last one. `MX_TEST_END` prints nothing under mxlink, nor in the kernel itself (a
fatal error of the VM), only the statistics: a host
that stopped at it would go away while the program still ends, with its files on `host:`. A program
started without mxlink (PCSX2 with `-elf`, ps2link) prints it there, at `MX_TEST_END` or else at
`mx_exit`, so a game needs none of its own there either.

When a program is back with the IOP as mxlink left it, mxlink cleans up after it: open files, sockets,
pad ports, the sound. Each of those calls gives up after a second; a server of the IOP that does not
answer means an IOP reset, with the modules loaded again, as after a program that loaded its own
(the screen says "the IOP did not answer: reset"). An IOP that does not answer at all cannot be reset
from the EE: the PS2 must be switched off then.

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

`host:` takes the fast path when a `ps2client` has said HELLO: its frames go past lwIP
(`common/include/mxfast.h`), data straight between SMAP and EE memory. On maxsdk's drivers it is a
module of its own, `iop/tcpip/mxfast`, a client of mxeth that builds its own frames, lwIP not used at
all; on ps2sdk's it is part of `ps2ip-nm` (`iop/tcpip/mxnet`), with its requests on a lwIP socket. Any other path, and `host:`
without HELLO, takes the driver through ioman. In 32 KiB, measured on a PS2: 7.6 MiB/s reading and
4.6 MiB/s writing on `host:`, against 2.1 and 1.6 through FILEIO (mxfast on mxeth: a 32 KiB read 4.0 ms,
a write 6.4 ms). On USB the PS2's host controller carries 937 KiB/s
(15 packets of 64 bytes a frame, the same on two different sticks); `iop/usb/usbmass_bd` queues a whole command at once and reads
64 KiB at 922 KiB/s on the IOP, against 719 for ps2sdk's driver. A program reading 64 KiB a call gets
903 KiB/s through mxio, 843 at 16 KiB. Writes reach 706 KiB/s at 16 KiB and 761 at 32 KiB (mxio's
largest write): they take about 12.8 packets a frame, and a write's CSW is only asked for
after its data. The memory card is limited by the device.
