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

The kernel is `apps/mxos`, resident at 0x88000-0xFF800. `libmxk` calls it through
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
| Pad | `<mx/pad.h>` | the kernel's (version 6, `ee/src/base/pad.c`'s reading in the kernel's memory): it opens the ports when a program asks, applies the pad's settings (ports swapped, Cross and Circle swapped: `apps/mxos/ee/pad.c`) and closes them when the program ends; on a kernel of version 5 the program's own, over the kernel's SIF. With `mxpad.irx` on the card also the USB pads (`common/include/mxpad.h`: their state written by SIF DMA into an area above the kernel's code), after the SIO2 pads; the home button (`MX_PAD_HOME`) of any pad goes to every port. A program built before `MX_PAD_HOME` ends on the combination only, so on a SIO2 pad it no longer ends by the combination there (rebuild it). Version 11: `mx_pad_rumble` (`pad_rumble`) drives a USB or Bluetooth pad's motors through `mxpad.irx`, a SIO2 DualShock's through PADMAN (`mx__pad_act`: DualShock mode, the motors' alignment once, then their values, each when the pad is done with its last request); the kernel calls the IOP only on a change, and stops every motor a program left running at its end. A SIO2 pad holds its port for half a second after it was last seen, and one that was there before for 1.5 s after a program opens the ports (`pad_init`), so the pads after it keep their ports. A SIO2 pad counts only with the configuration mode (DualShock, DualShock 2: PADMAN's STABLE). The kernel tells `mxpad.irx` the port of each USB or Bluetooth pad when it changes (`MXPAD_PLAYERS`), and the pad's light shows it |
| Display, home menu | `<mx/display.h>`, `<mx/gs.h>` | the kernel's (version 7): maxsdk's one writer of the display registers (`apply`, `ee/src/gs/gs.c`) hands them to the kernel, which writes and keeps them, and the video mode after SetGsCrt. Select + Start + L1 + R1 ask for the home menu (`apps/mxos/ee/home.c`): the program stops at its next `mx_frame_begin_n` with the GS idle (`mx_frame_sync`), lends 35 pages of GS memory (free, else the texture cache's, dropped: `ee/src/gs/home.c`) and waits in `home_run`. The menu is drawn by the CPU and copied by IMAGE transfers through the GIF FIFO (no drawing register changes), shown on circuit 1 over the program's last picture. Not in hires mode, nor for a program that does not come within 2 s: the combination then ends it |
| Sound | `<mx/snd.h>` | program's own, over the kernel's SIF: maxsdk's `mxsnd.irx` (`iop/sound/mxsnd`, `common/include/mxsnd.h`) on libsd. The kernel boots mxsnd (with libsd): programs bind to it and load nothing (libmx carries both for a program without the kernel). At a program's end the kernel calls `MXSND_STOP` with its physical range: mxsnd's status writes into the program's memory (every 10.7 ms) stop, and its voices and stream are silenced, only when the status area is in that range (another program's sound, one beside it, goes on). mxsnd serves one program at a time: a second one's `mx_snd_init` takes it over |
| Threads | `<mx/thread.h>`, `<mx/sched.h>` | the kernel's (version 10, below): threads, semaphores and interrupt handlers; `mx_sleep_us` takes EE timer 0, which `mx_exit` stops |
| Network | `<mx/net.h>` | a service on the IOP, `iop/tcpip/mxsock`, loaded by mxlink when it is on the card, on lwIP over maxsdk's drivers (mxdev9, mxeth, mxnet); closed when a program ends. `mx_net_wait` sleeps at one of its four wait services (at most 100 ms a call); with an older mxsock it asks every millisecond |

The program's first call gives the kernel its core memory after `_end` and its VM part;
the heaps and the program break work in those. It also gives the program an ASID of its own
(1-255 in turn; `mx__asid_set`, `apps/mxos/ee/exec.c`): the VM's entries are written under
it and not global, so they match only that program's address space (`docs/mx-os-plan.md`,
step 2b). The kernel is at ASID 0 again at the program's end (`tlb_restore`). Programs not on
`libmxk` stay at ASID 0.

From version 9 a `libmxk` program carries a note (`PT_NOTE`, name "MXK", `src/base/kstubs.c`):
the version it was built for and the address of its `mx__core_off`. One linked into VM gets
its core at a physical run of its own before its load (`core_place`, `apps/mxos/ee/exec.c`):
[0x10_0000, its end + 256 KiB of core heap) rounded to 1 MiB, at the top of main memory, mapped
by entries of its ASID in the slots of the BIOS's cached entries from 0x10_0000
(`tlb_core_map`, `apps/mxos/ee/tlb.c`; 4 MiB, 1 MiB or 256 KiB pages, at most 8). The kernel
writes the offset into the program's `mx__core_off` after the load and keeps it in its own while
the program runs, so `mx_paddr_of` and `MX_UNCACHED` / `MX_UCAB` translate the program's
pointers in both. The VM's frames are the memory below the run (`mx__vm_frames`), the core heap
ends with it. The main stack of a program linked into VM is in its core (`.stack`, 128 KiB or
`--defsym=__mx_stack_size=<bytes>`, in `ee/ld/maxsdk-vm*.ld`), not at the top of memory, so it
moves with it; a program without VM has it there too (`ee/ld/maxsdk.ld`), its heap from `_end` to
the top of memory. A `--defsym` of `__stack_size` itself gives crt0 a stack larger than `.stack`,
which runs into the heap: the start-up stops with a message. A program without VM, of
an older version, or too large stays at its own addresses. A program without VM gets a run only
when it asks for one with `MX_CORE_HEAP(bytes)` (`<mx/loader.h>`, a second note): its core heap
is then that much and no more.

A program with its core at a run of its own and without VM that calls `mx_loader_launch` stays
(`exec_park`, `apps/mxos/ee/exec.c`; `docs/mx-os-plan.md`, step 2c): the kernel keeps its own
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

Version 12: `mx_loader_launch_beside` (`launch_beside`) parks it the same way but leaves its
other threads and its interrupt handlers as they are: they go on beside the other (`apps/mxos/ee/kproc.c`;
`docs/mx-os-apps-plan.md`, step 2e-2). Both cores are in the TLB at once, each under its ASID
(the side's entries are kept in the first core slots, the other's come after them; `tlb_restore`
then leaves out the BIOS's entries over the side's range). A switch between the two sets the
ASID and `mx__core_off`, and exchanges the part of the kernel's state of a program that each
has of its own (`ee/resident.ld`'s `__proc_*`: the heaps, memory, exceptions, threads, the pad;
about 4 KiB) with the side's copy; the rest (the VM, the image, the home menu, every DMA line)
is the one in front's. The scheduler switches when the next thread is of the other process,
VU0's registers with it (not the accumulator); the kernel's own threads (the command thread
acts on the one in front) and the kernel's interrupt handlers (the VM's vblank) run in the
front's, a program's handlers in their process's. A cause or channel that a program turns off
(its `irq.c` does when its own slot is empty, its end does for its handlers) stays on while a
handler of another process is on it: the side's vblank, the kernel's SIF wake. Not yet: the side
ending, crashing or using the pad while the other runs.

Version 13: processes and messages (`<mx/proc.h>`, `<mx/msg.h>`; `apps/mxos/ee/kmsg.c`, `exec.c`;
`docs/mx-os-apps-plan.md`, step 2e-3). `proc_self` is the calling thread's process, `proc_list`
the program in front and the one beside it or waiting for it (id, state, core run, path).
Ports have names of up to 15 characters, 8 at once. A message is copied twice, each time in
the thread of the process it belongs to and so in its address space (threads run in user
mode: the kernel cannot reach another's memory): the request into one of 4 slots of 256 bytes
in the kernel's memory, which every process sees, and out of it in the receiver's
`mx_msg_receive`; the reply the same way back in that slot. Senders wait for a free slot,
receivers on their port's semaphore (their own process's: it goes with them), a sender on its
slot's for the reply. A process's end (`end_cleanup`, before its semaphores) takes its ports,
answers the requests to them with -2, drops its requests not yet received and marks those
received, so that their reply only frees the slot. The slots are what fits below the kernel's
code now; step 2e-4, which moves the command thread out of the kernel, gives them more room.

Version 14: `proc_spawn`, `proc_wait`, `proc_kill` (`<mx/proc.h>`) and `sleep_us`. `proc_spawn` is
`launch_beside` without the waiting thread: the caller comes back once the other runs (the
command thread's `exec_park_started`), with its process, and goes on beside it; -1 if it did
not start. `proc_wait` waits for (or, with `nohang`, asks about) the end of the program it
started, and gives its exit status (the last one's, also after its end). `proc_kill` ends
the program in front for another process the way its `mx_exit` would, without the restart
(which would end the caller too): the command thread's `program_ended`. `mx_sleep_us` is the
kernel's (`sleep_us`): timer 0 is one for all processes, and a program's own (as before
version 14) set it for itself and stopped it at its end, so the other's sleepers were never
woken. Since version 19 it is one of the kernel's software timers (below). A program's arguments are copied into its own memory at its
start: the kernel's are the next program's once it starts another.

Version 15: `proc_events` (`mx_proc_events`): the kernel's events to a port of the process
(`kmsg.c`; up to 4 processes have one). An event is a message without a sender (`from` 0):
the kernel puts it in a free slot without waiting (`kmsg_post`, from the command thread; it
is dropped when all slots are taken) and the slot is free again once received, no reply
wanted. The first: `MX_EVENT_END`, a program it started ended (its process and exit status),
at `exec_unpark`. A process's end takes its registration; a port that goes takes its
events not yet received.

Version 16: the kernel's log (`<mx/log.h>`, `log_read`; `apps/mxos/ee/klog.c`). Every
process's tty output (libmxk's `mx_printf` comes through the kernel's `mx_tty_write`) and the
kernel's own (maxsdk's `debug.c` calls the kernel's `mx__debug_tap`) go into a ring of 8 KiB as
pieces of up to 64 bytes, each with its process (0: the kernel), before they go on to the tty
as before. A piece is copied in the writer's thread, with interrupts on (its text may be in
VM), and out in the reader's (`mx_log_read`, from its own position; with `wait` it sleeps
until the next write). A reader that fell more than the ring behind goes on from the oldest
piece. Room for it: the kernel builds its maxsdk with a debug hold buffer of 256 bytes
(`MX_DEBUG_HOLD_BYTES`; it never holds), 16 KiB less.

Version 17: IOP channels (`mx_iop_channel`, `<mx/iop.h>`; `kmsg.c`). A process gives a SIF
command id of the user range (0-31) to a port of its own, at most 4 at once, one process per
id. maxsdk's SIF0 handler (`src/base/sif.c`) hands a command that is not the SIF's own to the
kernel's `mx__sif_cmd`, in the interrupt, which posts its payload (the packet without its
16-byte header, up to 96 bytes) to that port as a message from process 0, without waiting.
This replaces a fixed EE address for what an IOP module sends (ps2link's packets to
`0xFF800`, step 2e-4). A process's end gives its ids up.

Version 18: services (`mx_proc_service`; `apps/mxos/ee/main.c`, `exec.c`; step 2e-4). A
program with its core at a run of its own and without VM becomes the one beside for good: the
kernel parks it as for `launch_beside` (`park.service`) and starts the program in front that
would have come next (the shell; on PCSX2 `host:mxload.txt`'s). When that one ends the
service is not unparked: the end is cleaned up as usual, the service gets `MX_EVENT_END`, and
the next program in front starts (the shell) unless the service starts one (its
`mx_proc_spawn` starts a program in front when none runs; -1 while one does). At the boot,
and after a restart, the kernel starts the service in `mc0:/MXOS/SERVICES.CFG` (a path and
its arguments; on PCSX2 `host:services.txt`, the runner's `MXSERVICE`) in place of the shell.
With a service beside, the kernel prints no test end marker: it is the service's (2e-4:
mxlink's, for the host's runs).

Version 19: timers are the kernel's service (`<mx/timer.h>`, `src/irq/timer.c`; the kernel
builds its maxsdk with `MX_SOFT_TIMER=1`). Software timers (`mx_timer_add`, once or every
period, and `mx_sleep_us`'s sleepers) are slots of one table, each due at a COP0 Count, on EE
timer 1, which no program sees: it is set, one-shot, to the earliest, and its interrupt runs
what is due. A callback runs in its process's address space (`mx__timer_enter`, `kproc.c`), so
it and its argument are in core. The other EE timer, 0, is lent out whole (`mx_timer_claim`,
`mx_timer_release`) for a clock of its own: `mx_hires` counts H-blanks with it (the kernel's
sleep on timer 0, versions 14 to 18, rewrote it under the shell). Timers 2 and 3 are the
BIOS's page, read-only to programs. A process's end takes its timers, sleepers and claim
(`mx__timer_reap`, after `units_off` stopped the EE timers, which sets the kernel's again).
`mx_timer_epc` gives a callback where the interrupted code was (the profiler, a software
timer now). The old `mx_timer_periodic(t, ...)` is a software timer too. A program of its own
(`libmx`) has the same calls in `timer.c`, on timer 0, with timer 1 to lend out.

Version 20: the host link is a service (`apps/mxlink/ee`, step 2e-4). ps2link.irx still sends
ps2client's commands by SIF DMA to `0xFF800` (a SIF command packet is too small for execee's
arguments, and the DMA arrives whatever program runs); the kernel's command thread passes
execee and dumpreg (ps2client's ping) on to the port of the process with the IOP channel
`MX_IOP_CHANNEL_HOST` (18, `<mx/iop.h>`): the packet up to its last string's 0. The kernel keeps
the reset, dumpmem, writemem, gsexec, the VU commands and the IOP's exception, and serves all of
them while no process has the channel (no service, a program that took all memory). A service's `mx_proc_kill` or `mx_proc_spawn` of the program in
front means the host took over: the text screen after it, not the shell, until the next reset,
and a shell it ended keeps the IOP as the old takeover did (cleaned up, not reset: reading the
modules again from the card failed after the shell's drivers). A service with nothing in front
reports ready; one that ends before it calls `mx_proc_service` is not started again until the
next reset. The kernel's file calls are one at a time for every process's threads (the lock of
`sif.c`, made by the kernel, given back for a thread that ended in a call): a service's tty
write and the kernel's own ran into each other and sent a packet twice.

Version 21: the EE's physical memory for a process (`mx_phys_read`, `mx_phys_write` in
`<mx/proc.h>`; `apps/mxos/ee/kirq.c`). Threads run in user mode under their process's ASID, so
the kernel copies in kernel mode: its poke syscall copies up to 4 KiB a call between physical
memory (kseg0) and the caller's core (touched in user mode first: a buffer that is not the
caller's faults in its thread, not in the syscall). The buffer is core, not VM; writing is a
service's only, and the caches are written back and emptied after it. With it the host link's
app serves dumpmem and writemem (physical addresses, as the kernel saw them before), and gsexec
and the VU commands too; the kernel keeps the reset and the IOP's exception. A shell the service
ends always gets the restart (as the takeover did): ended without it, the next program crashed
at once, before its first line, when the shell had not changed the IOP (something of the shell's
went on; not found yet). Test: `k43_phys`. Without the app (no service, a program that took
all memory) the kernel serves only the reset, execee,
dumpreg (ps2client's ping) and the IOP's exception; the other commands are the app's only.

Another kernel replaces this one (2026-11-11, mx OS plan step 2h-2; `apps/mxos/ee/exec.c`,
`kreplace.S`): an ELF whose segments lie below 1 MiB over the running kernel (a new
`MXKERNEL.ELF`, `ps2client execee host:MXKERNEL.ELF`) is read to 0x0100_0000, a table of its
segments and a trampoline go to 0x00F0_0000, and ExecPS2 starts the trampoline, which copies the
segments over the old kernel, clears their bss, writes the data cache back, drops the
instruction cache and jumps to the new kernel's entry with ExecPS2's arguments. It boots as after
a restart (the IOP reset, the services and the shell started again). Before, a new kernel was
tried by starting mxkernel-high (a copy linked at 0x1E0_0000) and from it `MXKERNEL.ELF`. Once the
BIOS is gone (2h-5) the kernel starts the trampoline itself instead of ExecPS2.

The kernel runs MXOS programs only (2026-11-11, mx OS plan step 2h-2; `apps/mxos/ee/exec.c`):
an ELF without the MXK note of version 10 or later (a standalone ELF on `libmx`, ps2sdk's, an
older `libmxk`) and a loader (an ELF below 1 MiB that is not a kernel replacing this one, such as
ps2link) are refused with a message; mxboot, a legacy loader or PCSX2's `-elf` starts those. Gone
with them: the kernel's restart with the BIOS's threads for such a program (`bios_mode`), the
loader's start by ExecPS2 (so mxkernel-high, itself a standalone ELF, no longer starts), and the
replaced BIOS `Exit` (syscall 4) for programs that did not return through the kernel.

The BIOS syscalls a program still makes are counted (2026-11-11, mx OS plan step 2h-3;
`apps/mxos/ee/ksys.S`, `kirq.c`): the kernel points the BIOS's exception table entry for
syscalls (0x8001_5360, `docs/bios.md`) at a stub that counts, by `v1 & 0xFF` (an i-call's negative
number at 0x81-0xFF), the syscalls of the program the kernel started last (the current ASID its,
EPC in its own code: not the kernel's below 1 MiB, not the BIOS's in kseg0/kseg1), then goes on
into the BIOS's dispatcher; the table is the BIOS's again whenever the interrupt table is
(`kirq_off`). At the program's end the log has `BIOS syscalls: N, by number:` and the numbers
with their counts (calls made through the kernel's services are the kernel's and not counted).

Version 22: the video timing without the BIOS (plan step 2g; `apps/mxos/ee/kvideo.c`), and
`iop_write` (below). maxsdk
computes the CRTC registers (SMODE1 with the PLL, SYNCH1, SYNCH2, SYNCV, SRFSH, SMODE2) from a
description of the line and the frame (`<mx/crtc.h>`, `ee/src/gs/crtc.c`); the descriptions of
the BIOS's modes give the values its SetGsCrt writes, all 20 of them (`tests/host/test_crtc.c`),
and new ones are made the same way (`mx_video_720p50`, `mx_video_1080i50`, `mx_video_1080p30`,
or any `struct mx_crtc` in `mx_video_mode.crtc`). `mx_video_set` hands the description to the
kernel (`crtc_set`), which writes it as SetGsCrt does: the HD clock switch (0xBF80146C bit 3,
for the 74.25 MHz modes), the registers in its order (PRST and SINT), and the video encoder on
the I2C bus at 0xBA000000, with the registers SetGsCrt writes for that kind of mode. Those last
two were read from the BIOS of one console; the kernel takes this path only where the BIOS's
boot variables are that console's (0x8002_2580 = 0x32, the encoder's id 0x1C, 0x8002_2618 = 0),
else, and for a program before version 22, SetGsCrt as before (a mode it does not have, code 0,
is then refused: `mx_video_set` returns -1). The kernel's own screen uses the same path.
Test: `examples/display` (on libmxk), which also counts each mode's vblanks a second.

SIF1 DMA without the BIOS (`ee/src/base/sifdma.c`): once the kernel's threads are on, its first
SIF send takes channel 6 when it is idle and runs a ring of 31 tags of its own, and gives it back to
the BIOS's ring before an ExecPS2. As the BIOS's SifSetDma, each transfer's first quadwords (at most
7) are copied after the IOP tag at the call, from a physical address too: a REF to a small buffer of
the caller's was read by the DMA after the caller had used it again, and the IOP, given another
call's data, answered no more (on the PS2; PCSX2 does the DMA at once). A running chain is appended
to while the DMAC is suspended. The registers of channels 5-7 (the page at 0x1000_C000) are stored
in kernel mode, through the kernel's syscall (an interrupt handler: through kseg1), since the BIOS's
TLB maps that page read only for user mode (`bios.md`). Once the ring is the kernel's, SIF0's chain
and the SIF registers 1-4 are written by maxsdk too; until then, and in every program on the BIOS's
threads, all of it goes to the BIOS (`iSifSetDma`, `iSifSetDChain` from an interrupt handler).
`libmxk`'s `mx_iop_write` goes through the kernel (version 22: `iop_write`, a physical source):
a program's own SifSetDma would meet the kernel's ring. A SIF wait of over 3 s shows the ring's
state on the kernel's screen, and a crash of the kernel shows there at once, before the report
goes out through the SIF.

Two things a process beside showed: a program's end waits only for the IOP calls of its own
threads, those that are gone (`mx__sif_calls_finish` keeps the calls and packets of threads
still alive: a service's tty write was waited for until it gave up, and its packet was
freed). An interrupt handler whose code is the kernel's (below 1 MiB, its `irq.c`: the SIF
waits' vblank wake, added in a program's thread at the first call that needed it) belongs to
process 0, not to that program, so its end cannot turn the line off under it.

## Interrupts and threads

The kernel is first at every INTC and DMAC interrupt (`apps/mxos/ee/kirq.S`, `kirq.c`;
`docs/bios.md`): the BIOS's interrupt table at 0x8001_5380 points its entries 2 and 3 at the
kernel's entry, through kseg0, which saves the registers C may change and runs `kirq()` on a
stack of its own. Only while the kernel or a program with its note runs: before a program
without it or a loader (ExecPS2) the table is the BIOS's again, so one that loads another
loader over the kernel finds the BIOS as it was.

From version 10 the threads, semaphores and interrupt handlers are the kernel's
(`apps/mxos/ee/ksched.c`, `ksched.S`; `docs/mx-os-plan.md`, step 2d-2). Every thread of the
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
  for the BIOS thread (the kernel's). Since 2026-11-11 (plan step 2g) a program on the kernel's
  threads calls neither `SetupThread` nor `SetupHeap`: crt0 sets the main stack itself (its
  top less the 0x2A0 bytes `SetupThread` used above it), and the start-up ends the heap at that
  stack (the top of memory when the stack is inside the program). A program on the BIOS's
  threads, and `libmxk` built before, still call both; for those the kernel sets the carrier's
  argc to 0 before each program, so `SetupThread` copies nothing (it read the block of the
  program before, at an address the next one need not have: `docs/bios.md`).
A program that is not on `libmxk` of version 10 (ps2sdk's, `libmx`, an older `libmxk`) is not
started (2h-2, above); before, the kernel started again with the BIOS's threads for it.

## The end of a program

A `libmxk` program (one that called the kernel's `program` service) ends **without a restart**
(version 8, `apps/mxos/ee/end.c`): its way back (`mx_exit`, a crash, BIOS `Exit`) wakes the
command thread and sleeps; the command thread then does what ExecPS2 did:
- the program's threads and semaphores are terminated and deleted: on the kernel's threads those
  of its process (a thread or semaphore is the process's of the thread that made it; a program's
  first thread gets a process of its own, `apps/mxos/ee/ksched.c`), on the BIOS's those that
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
A program that changed the IOP (loaded modules), an IOP that does not answer,
`mx_loader_launch` and the host's reset still take the restart (programs not on `libmxk`, and
those before version 8, which did not tell the kernel their interrupt slots, took it too until
2h-2 refused them):

Each such program ends in `ExecPS2`, which starts the kernel again but does not load it: `.bss`
is cleared by the start-up, and `.data` is restored from a copy by `mxlink_start`
(`ee/hook.S`, which fills its branches' delay slots by hand: under the assembler's reorder
mode the copy's pointer did not move, and every restart, the first boot's included, came back
with `.data` wrong), so all start values are as linked. What must survive is in the section
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

`make -C apps/mxos emu` builds the kernel for PCSX2 (`build/apps/mxos-emu/mxkernel.elf`,
`MX_LOADER` in `mk/maxsdk.mk`): no network, no IOP modules, no commands. Its command thread
starts the program in `host:mxload.txt` (path and arguments, in words) from the directory of
the ELF, as on the PS2, so programs end without a restart and start others (`launch`,
`launch_beside`). Run it with `PCSX2_EE=interp`. PCSX2's TLB does not look at the ASID (of
two entries at one address, the last written is used), so at a switch between processes this
kernel writes the next one's entries again (`tlb_emu_asid`).

## Crashes

The kernel's exception vector stays installed while a program runs. It reports what the
VM cannot handle: registers, stack and the `trace:` line go to the host, on the kernel's
stack. A crash inside a file call does not block the report: the file lock is given up.
The program ends with status 255 and the kernel restarts.

The report survives the restart in the kernel's `.data`. `mx_crash_last` gives it to
the next program (the shell shows it, `libmxk` in core), and the text screen shows it. It
belongs to the program that ended last: the next return replaces it, and a reset drops it.
A shell that crashes, also before its first line, is not started again until a reset: the
kernel shows its text screen (not the shell crashing at each start).

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
