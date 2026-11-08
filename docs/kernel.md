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
| Pad | `<mx/pad.h>` | the kernel's (version 6, `ee/src/base/pad.c`'s reading in the kernel's memory): it opens the ports when a program asks, applies the pad's settings (ports swapped, Cross and Circle swapped: `tools/mxlink/ee/pad.c`) and closes them when the program ends; on a kernel of version 5 the program's own, over the kernel's SIF. With `mxpad.irx` on the card also the USB pads (`common/include/mxpad.h`: their state written by SIF DMA into an area above the kernel's code), after the SIO2 pads; the home button (`MX_PAD_HOME`) of any pad goes to every port. A program built before `MX_PAD_HOME` ends on the combination only, so on a SIO2 pad it no longer ends by the combination there (rebuild it) |
| Display, home menu | `<mx/display.h>`, `<mx/gs.h>` | the kernel's (version 7): maxsdk's one writer of the display registers (`apply`, `ee/src/gs/gs.c`) hands them to the kernel, which writes and keeps them, and the video mode after SetGsCrt. Select + Start + L1 + R1 ask for the home menu (`tools/mxlink/ee/home.c`): the program stops at its next `mx_frame_begin_n` with the GS idle (`mx_frame_sync`), lends 35 pages of GS memory (free, else the texture cache's, dropped: `ee/src/gs/home.c`) and waits in `home_run`. The menu is drawn by the CPU and copied by IMAGE transfers through the GIF FIFO (no drawing register changes), shown on circuit 1 over the program's last picture. Not in hires mode, nor for a program that does not come within 2 s: the combination then ends it |
| Sound | `<mx/snd.h>` | program's own, over the kernel's SIF |
| Threads | `<mx/thread.h>`, `<mx/sched.h>` | the kernel's (version 10, below): threads, semaphores and interrupt handlers; `mx_sleep_us` takes EE timer 0, which `mx_exit` stops |
| Network | `<mx/net.h>` | a service on the IOP, `iop/tcpip/mxsock`, loaded by mxlink when it is on the card, on lwIP over maxsdk's drivers (mxdev9, mxeth, mxnet); closed when a program ends. `mx_net_wait` sleeps at one of its four wait services (at most 100 ms a call); with an older mxsock it asks every millisecond |

The program's first call gives the kernel its core memory after `_end` and its VM part;
the heaps and the program break work in those. It also gives the program an ASID of its own
(1-255 in turn; `mx__asid_set`, `tools/mxlink/ee/exec.c`): the VM's entries are written under
it and not global, so they match only that program's address space (`docs/mx-os-plan.md`,
step 2b). The kernel is at ASID 0 again at the program's end (`tlb_restore`). Programs not on
`libmxk` stay at ASID 0.

From version 9 a `libmxk` program carries a note (`PT_NOTE`, name "MXK", `src/base/kstubs.c`):
the version it was built for and the address of its `mx__core_off`. One linked into VM gets
its core at a physical run of its own before its load (`core_place`, `tools/mxlink/ee/exec.c`):
[0x10_0000, its end + 256 KiB of core heap) rounded to 1 MiB, at the top of main memory, mapped
by entries of its ASID in the slots of the BIOS's cached entries from 0x10_0000
(`tlb_core_map`, `tools/mxlink/ee/tlb.c`; 4 MiB, 1 MiB or 256 KiB pages, at most 8). The kernel
writes the offset into the program's `mx__core_off` after the load and keeps it in its own while
the program runs, so `mx_paddr_of` and `MX_UNCACHED` / `MX_UCAB` translate the program's
pointers in both. The VM's frames are the memory below the run (`mx__vm_frames`), the core heap
ends with it. The main stack of a program linked into VM is in its core (`.stack`, 128 KiB, in
`ee/ld/maxsdk-vm*.ld`), not at the top of memory, so it moves with it. A program without VM, of
an older version, or too large stays at its own addresses. A program without VM gets a run only
when it asks for one with `MX_CORE_HEAP(bytes)` (`<mx/loader.h>`, a second note): its core heap
is then that much and no more.

A program with its core at a run of its own and without VM that calls `mx_loader_launch` stays
(`exec_park`, `tools/mxlink/ee/exec.c`; `docs/mx-os-plan.md`, step 2c): the kernel keeps its own
state of it (the `.data` and `.bss` of `ee/resident.ld`'s program part) in the program's core
heap, takes its interrupt handlers from the BIOS (the slots' entry functions are in
`irq_tables`, version 9), closes the pad's ports, suspends its other threads and starts the
other with the kernel's state as linked, its run below the waiting one's. At the other's end
(or when it could not start) the waiting program is mapped again under its ASID, its kernel
state put back (the pad's state forgotten: its next `mx_pad_init` opens the ports), its
handlers added again with their new ids in its slots, its threads resumed, and its call
returns the other's exit status (-1: not started). An other that cannot have a run of its own
takes all memory: the waiting program is given up and ends with it (its threads and semaphores
go with the other's), and mxlink goes on as before (the shell starts again). The kernel's VM state is cleared by the
restart after each program.

## Interrupts and threads

The kernel is first at every INTC and DMAC interrupt (`tools/mxlink/ee/kirq.S`, `kirq.c`;
`docs/bios.md`): the BIOS's interrupt table at 0x8001_5380 points its entries 2 and 3 at the
kernel's entry, through kseg0, which saves the registers C may change and runs `kirq()` on a
stack of its own. Only while the kernel or a program with its note runs: before a program
without it or a loader (ExecPS2) the table is the BIOS's again, so one that loads another
loader over the kernel finds the BIOS as it was.

From version 10 the threads, semaphores and interrupt handlers are the kernel's
(`tools/mxlink/ee/ksched.c`, `ksched.S`; `docs/mx-os-plan.md`, step 2d-2). Every thread of the
kernel and of a `libmxk` program of version 10 is a kernel thread, all on one BIOS thread,
which the BIOS never switches. maxsdk's `mx_bios_*` calls for them (`ee/src/base/ksys.c`,
generated by `ee/gen/gen.py` from the K entries of `hw_desc.py`) go to the kernel's table
(`kthreads` in `struct mxk_services`) when the kernel has one, else to the BIOS:
- threads as the BIOS's (24 threads, 128 semaphores, 32 handlers): the lower priority first,
  equal ones in turn (`rotate`), sleep and
  wake-up counts, suspend and resume, `ReferThreadStatus` with the BIOS's values; an idle thread
  when none is ready. The whole context is switched: the 128-bit GPRs, HI/LO, HI1/LO1, SA, the
  FPU with its accumulator, EPC and the interrupt state (EIE and IE);
- the switch happens in the kernel's calls (a wait, a wake-up of a better thread: a syscall of
  the kernel's own, 0x3B) and at the end of an interrupt that made a better thread ready, when
  it came from user code (ERET into the next thread);
- the kernel owns both interrupt lines: its dispatch acknowledges each pending cause or channel
  that has a handler and runs its handlers in the BIOS's order (`next` 0 first, -1 last, else
  before that one; a handler returning -1 skips the rest), until none is left. What is pending
  without a handler is acknowledged and dropped; nothing goes on into the BIOS's handler. The
  order matters: the kernel's own GS handler (its screen) acknowledges FINISH, so a program's,
  added later, comes before it;
- `EnableIntc` / `EnableDmac` and the `Disable` ones write I_MASK and D_STAT's CIM bits
  themselves (the BIOS's leave a cause off that has no handler of its own: `docs/bios.md`);
- a program's arguments come from the loader's block, not from `SetupThread`, which answers
  for the BIOS thread (the kernel's).
A program that is not on `libmxk` of version 10 (ps2sdk's, `libmx`, an older `libmxk`) calls the
BIOS's threads itself: mxlink starts again (ExecPS2) with the BIOS's threads before it
(`bios_mode`, `tools/mxlink/ee/main.c`), and with its own again before the next program of
version 10, the launch's arguments, swap file and the shell kept over the restart.

## The end of a program

A `libmxk` program (one that called the kernel's `program` service) ends **without a restart**
(version 8, `tools/mxlink/ee/end.c`): its way back (`mx_exit`, a crash, BIOS `Exit`) wakes the
command thread and sleeps; the command thread then does what ExecPS2 did:
- the program's threads and semaphores are terminated and deleted: on the kernel's threads those
  of its process (a thread or semaphore is the process's of the thread that made it; a program's
  first thread gets a process of its own, `tools/mxlink/ee/ksched.c`), on the BIOS's those that
  were not there just before the program started (the program's, the VM's pager and writer, its clients' semaphores);
- the program's interrupt handlers are removed: its `libmxk` told the kernel where its slots
  are (`irq_tables`), the kernel reads their BIOS ids and runs none of the program's code; a
  cause or channel without a handler of the kernel's own is disabled;
- the calls to the IOP that its threads were waiting for are waited for, and every RPC packet
  back (`mx__sif_calls_finish`, at most about 2 s): a late answer names a client in the
  program's memory, where the next program is loaded;
- what the program left in the kernel's SIF layer goes (`mx__sif_program_end`: sleepers and
  servers in its memory, descriptor positions, the fio lock); the kernel's own clients whose
  semaphore went with the program's make a new one; the VM's and the home menu's vblank
  functions are taken off;
- the units: DMA channels but SIF's stopped, VIF0/VIF1, GIF, IPU, VU0/VU1 reset, timers 0 and 1
  stopped; the data cache written back and both caches emptied (else the program's dirty
  lines land over the next program after its load); the TLB written back;
- the IOP's cleanup as before (below); then the kernel's state of the program (the `.data` and
  `.bss` of the VM, heaps, exceptions, threads, the pad and the home menu, apart in
  `ee/resident.ld`) is set back as linked, and the screen comes back with its log.
A program that changed the IOP (loaded modules), an IOP that does not answer, a program not on
`libmxk` (ps2sdk's, `libmx`), a `libmxk` program from before version 8 (it did not tell the
kernel its interrupt slots: its handlers would stay with the BIOS, in the next program's
memory), `mx_loader_launch` and the host's reset still take the restart:

Each such program ends in `ExecPS2`, which starts the kernel again but does not load it: `.bss`
is cleared by the start-up, and `.data` is restored from a copy by `mxlink_start`
(`ee/hook.S`), so all start values are as linked. What must survive is in the section
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
(`iop/tcpip/mxfast`, `docs/iop-drivers.md`); other clients get it by broadcast, as udptty sent it.
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
(`common/include/mxfast.h`), data straight between SMAP and EE memory: `iop/tcpip/mxfast`, a client
of mxeth that builds its own frames, lwIP not used at all. Any other path, and `host:`
without HELLO, takes the driver through ioman. In 32 KiB, measured on a PS2: 7.6 MiB/s reading and
4.6 MiB/s writing on `host:`, against 2.1 and 1.6 through FILEIO (mxfast on mxeth: a 32 KiB read 4.0 ms,
a write 6.4 ms). On USB the PS2's host controller carries 937 KiB/s
(15 packets of 64 bytes a frame, the same on two different sticks); `iop/usb/usbmass_bd` queues a whole command at once and reads
64 KiB at 922 KiB/s on the IOP, against 719 for ps2sdk's driver. A program reading 64 KiB a call gets
903 KiB/s through mxio, 843 at 16 KiB. Writes reach 706 KiB/s at 16 KiB and 761 at 32 KiB (mxio's
largest write): they take about 12.8 packets a frame, and a write's CSW is only asked for
after its data. The memory card is limited by the device. The internal HDD (`ata0:`) is
`iop/dev9/mxata` under bdm, as USB: speed on a PS2 not measured yet.
