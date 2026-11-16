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

The kernel is `apps/mxos`, resident in the last MiB of main memory (0x1F00000-0x1FFFFFF,
`<mx/cpu.h>`: `MX_KERNEL_LO`, `MX_KERNEL_HI`). `libmxk` calls it through
`struct mxk_services` (`<mx/kernel.h>`), a table the loader passes. Only `libmxk` sees how
a call reaches the kernel, so that can change without changing programs. Kernel code runs
in the caller's thread; the VM's writer and pager are threads of the kernel (the writer, started in the call of a program that got a space, is given to the process that has the VM's lock: `kvm_init`, vm.md). Functions are
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
(1-255 in turn, skipping those that a parked program, a program beside or a service still
has: `asid_new`, `apps/mxos/ee/exec.c`; before 2026-11-14 the turn came round to them after
255 starts and the new program's TLB entries were theirs): the VM's entries are written under
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
the program in front and those beside it or waiting for it (id, state, core run, path).
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
(which would end the caller too): the command thread's `program_ended`.

A process beside that ends (its `mx_exit`, or a crash in one of its threads) ends alone (mx OS
plan step 4c, 2026-11-14): `mxlink_return` sees that the caller is not the process in front,
records its status (`ptab.c`) and has the command thread take it away (`side_end_now`,
`exec_side_ended`): its threads, semaphores, interrupt handlers, ports, timers and the file
server's lock by its process tag (`end_side_off`), its slot out of the sides and park lists,
its VM space and pinned core back to the VM; the kernel's `mx__vm_shutdown` service does
nothing for it (its exit calls it). Its files, sockets and IOP memory are not tagged yet; its
core, when a run of its own, stays unused until those above it end. The one in front goes
on. `tests/ps2/k47_sideend.c` (exit and crash).

Version 24 (mx OS plan step 4d, 2026-11-14): `proc_front` (`mx_proc_front`). The command thread
(`exec_front_switch`) parks the program in front as if it waited for the other (its record
in its own heap, its handlers out, all its threads suspended, its VU0 registers kept in the
record) on top of the list, and brings the chosen one (waiting or beside, not the service)
in front as `exec_unpark` does, without the end of a child: its mapping, kernel state, VM
space, path and VU0 registers. When that one ends, the paused one is in front again; another
`proc_front` may bring it back first. `proc_wait` answers from the process table for a process
that is not the caller's last child. Since 4b the records are in the programs' own memory, so
the lists hold up to six waiting and four beside (TLB entries for the cores beside are the
limit in practice). `tests/ps2/k48_turns.c`, `tests/ps2/k49_three.c`. A process that waits
(paused, or for its child) is not mapped, so its handlers in the kernel's table do not run
(`kproc_handler_enter` answers -1 from the process table, `ksched_dispatch` skips them) and
its timers are put off by 10 ms until it is in front again (`tick` in `src/irq/timer.c`; a
one-shot one is not lost): before, a libgl program's timer ran in its child's memory and
crashed the kernel. `mx_sleep_us` is the
kernel's (`sleep_us`): timer 0 is one for all processes, and a program's own (as before
version 14) set it for itself and stopped it at its end, so the other's sleepers were never
woken. Since version 19 it is one of the kernel's software timers (below). A program's arguments are copied into its own memory at its
start: the kernel's are the next program's once it starts another.

Also version 28: a host's run on a swap file of its own on `host:` (`apps/mxos/ee/swap.c`'s
`kswap_host`, `kswap_host_end`). mxlink's app reads `host:mxswap.txt` ("host:<path> [MiB]", 128 by
default) at each execee and gives it to `mx_proc_spawn` as the program's swap file; for a
program the service starts in front, and only for a file on `host:`, the kernel detaches MXOS's
swap file (when it holds no page: the shell is ended for a host's run) and attaches that one
before the program's space is made, so the space has the 128 MiB of a VM with a swap file. When
the program ends, MXOS's file comes back where its device is. Without the file the run has
MXOS's swap file, if its device is there. A running program's ELF stays open (the VM's pages
come from it), so a new `MXLINK.ELF` cannot be written on the card while the service runs
(mcman refuses, -6): only while it does not.

Version 31 (2026-11-16): the video settings (`apps/mxos/ee/kvideo.c`, `con.c`,
`<mx/screen.h>`, `ee/src/gs/screen.c`). No new entries in the table: programs read the
`video.*` settings themselves (`mx_screen_conf`) and set their mode as before (`crtc_set`). The
kernel's screen is set up in `video.mode` (with `video.overscan`): 640 or 704 x the area's lines
(32-bit) for 240p, 288p (the font at its own height), 480i, 576i, 480p and 576p; 1280 x 720 and
1920 x 540 (FRAME, or 1080p's lines twice; both 16-bit) for 720p, 720p50 and the 1080 modes, 170
columns; again when the mode changes while it is shown. 720p50, 1080i50 and 1080p24/25/30 have
no BIOS code: where the kernel cannot set the CRTC itself (no known board, as on PCSX2) they fail
and the screen stays in the region's mode until the trial ends. `video.out` `rgb` clears
SMODE1.GCONT for 240p, 288p, 480i and 576i (RGB on the AV connector, as the BIOS's SetGsVParam 0; before,
always YCbCr). A change of `video.mode` or `video.out` that differs from `video.confirm` is on
trial for 15 s, at the start too, then the confirmed values (or the region's mode) come back
with a notification. L1 + L2 + R1 + R2 on a pad a program reads within 30 s of the start set
the region's 480i or 576i and confirm it. `mx_gs_init` in libmxk shows its w x h through
`mx_screen_pick` (exact size, the shape it has on the region's TV mode). On PCSX2 the screen is
set up again after the settings are read. `tests/ps2/k59_video.c`.

Version 34 (2026-11-16, mx OS plan step 5b): a program takes its graphics up again after
another process had them (`<mx/frame.h>`: `mx_frame_on_lost`; `src/gs/frame.c`'s
`mx__frame_lost`, `src/gs/home.c`, `apps/mxos/ee/end.c`'s `end_gfx_gen`).
- The kernel counts how often the graphics went to another process or were reset
  (`gfx_gen`): a change of the process in front, and the reset of the GIF, the VIFs and the
  DMA channels at the end of the one in front.
- The frame code compares the count at each frame boundary (after `front_wait`, and after
  `home_run` when the shell was in front). When it moved: VIF1 and the GIF reset, the ring's
  DMA set up again, every frame sent counted as drawn and shown (their FINISH and vblank may
  have gone to the other process), the GS's interrupt mask written again, VU0's and VU1's
  microprograms forgotten (`mx_frame_mpg` sends them again when next used), the texture
  buffers free and the texture cache empty (`mx_vcache_reset`, if the program has it), then
  the program's own function (`mx_frame_on_lost`): for what it keeps in GS memory itself
  over frames, and state it thinks the GS or VU1 still has.
- A program that lets another in front itself (`mx_proc_spawn`, `mx_proc_front`) first has
  its stream drawn (`mx__frame_yield`: `mx_frame_sync`, then up to 100 ms for the frames'
  FINISH), so the other finds VIF1 and the GIF idle.
- Not in hires mode (the passes run from interrupts): such a program still stops its
  graphics before it starts another and sets them up again after, as MXSHELL does.
- `tests/ps2/k66_lost.c`: a program's picture before and after a child with a stream of its
  own was in front and ended is the same, pixel for pixel (a SCPH-70004 and a SCPH-90006).
  Before, the parent's stream stood still there (`k65_frontwait`'s first form). Not yet run:
  the same over the home button with a shell that draws, and a program with textures and
  VU1 programs (libgl needs its own function for its state).

Version 33 (2026-11-16, mx OS plan step 5a): the graphics are the process in front's. The
frame code asks at each frame boundary (`mx__home_poll`, `front_wait`), and a process that is
not in front waits there, asleep, until it is: the one in front ended, or `mx_proc_front` or the
home button brought it there (`apps/mxos/ee/main.c`'s `kproc_front_wait`). So a shell that runs
beside the game it started draws nothing over it. A process that draws through the frame
pipeline only; one that writes the GS itself is not held. `tests/ps2/k65_frontwait.c` (a
frame's begin beside a child that sleeps two seconds returns after the child's end; on a
SCPH-70004 and a SCPH-90006). What it found for step 5b: when the one in front ends, the kernel
resets the GIF, the VIFs and the DMA channels (the owner ended), so the frame stream of the
process that is in front next no longer runs: it has to set its graphics up again.

Version 32 (2026-11-16, mx OS plan step 5): the picture of the program paused at the home
button, for the shell (`<mx/screen.h>`: `mx_home_picture`, `mx_home_picture_read`;
`apps/mxos/ee/kpic.c`, `src/gs/home.c`).
- When the kernel asks for the home button's turn at a frame boundary (`mx__home_poll`), the
  program's frame code, with the GS idle (`mx_frame_sync`), reads the buffer it shows back
  from GS memory (`mx__home_capture`: the `mx_gs.rt` that DISPFB2 names) into pinned memory
  of its own, and names it to the kernel (`home_picture_set`: size, format, physical
  address). When it goes on it takes the name back and frees the memory.
- The read-back is in strips of whole lines of about 256 KiB: a DMA transfer is at most
  65535 quadwords, and a 640 x 448 picture of 32 bits is more (`mx_gs_download` now refuses
  such a size; before, it sent the count's low 16 bits and the rest of the buffer stayed as
  it was).
- The shell, in front after its `MX_EVENT_HOME`, asks for the description
  (`mx_home_picture`: w, h, the GS's format, bytes, display lines a buffer line, the process)
  and copies the pixels (`mx_home_picture_read`, into any memory of its own: VM is fine). The
  kernel copies from the paused program's memory through `MX_PHYS_WIN`. There is a picture
  only while that program is paused, and only for the shell.
- None from a program in hires mode (`mx_frame_sync` refuses), one that does not use the
  frame pipeline, one whose shown buffer is not one of `mx_gs.rt`, or without memory for it
  (1.1 MiB for 640 x 448 in 32 bits).
- Tests: `tests/ps2/k64_homepic.c` (the read-back: every pixel of a known frame, on a
  SCPH-70004 and a SCPH-90006); `k50_home` run as the shell asks for the picture (on PCSX2,
  without the frame pipeline, it checks that there is none). The shell's read of a real
  picture has not run yet.

Also version 31: the kernel in the last MiB of main memory (2026-11-16, mx OS plan step 4a;
`apps/mxos/ee/resident.ld`, `tlb.c`). Before, it was at 0x88000-0xFF800, between the BIOS and
the programs, with 128 bytes of room left for code.
- Its code and constants are at 0x1F00000-0x1F7FFFF, its data, `.bss`, heap and main stack
  from 0x1F80000, the VM's tables in the last 64 KiB. Virtual = physical, in every address
  space.
- Two TLB entries of its own (21: the code, without the right to write; 40: the data), in
  place of the end of the BIOS's entry for 0x1800000-0x1FFFFFF. At the first start the kernel
  runs under that entry, so the two are written with nothing read or written between them.
  A program's core takes entries 13-20.
- The VM's frames are 0x100000-0x1EFFFFF (960 of them) while the BIOS is used; without it,
  see below.
- A program's core ends below the kernel (`MX_CORE_HI` is `MX_KERNEL_LO`), and `libmxk`
  takes the kernel's table only from the kernel's memory: **every program is built again**.
  One built before does not find this kernel and ends in the BIOS's Exit; this kernel's
  programs do not find a kernel from before.
- mxboot loads it as before (it runs in the last 256 KiB; what it loads of the kernel ends
  below that). A kernel from before does not start it from memory: to that one it is a
  program. The first time it is installed with everything beside it (the modules, the
  service, the shell) in a directory of its own, booted as mxboot's one-shot.
- `mxkernel-high` is gone.

Also version 31: no BIOS after the kernel's first start (2026-11-16, mx OS plan step 4a part
2; `apps/mxos/ee/kirq.c`'s `kirq_bios_leave`, `kvideo.c`, `exec.c`, `kreplace.h`). On a console
whose video the kernel sets itself (ROM 2.00's and 2.20's boards, `kvideo.c`); on another, and
on PCSX2, the BIOS stays for its SetGsCrt.
- At its first start the kernel still asks the BIOS: SetupThread, the SetSyscall for its way
  into kernel mode, the BIOS's tables and vectors, GetGsDxDyOffset for every mode, the board's
  variables. Then, before its own restart, it leaves: a syscall it has no handler of and an
  interrupt line that was the BIOS's go to the kernel's crash report, the BIOS's vectors are
  never put back, and nothing of the BIOS's memory is read or written again. What was read
  (the video's, the SIF's software registers) is kept over the kernel's restarts.
- A kernel replaces another without ExecPS2: the old one starts the trampoline in kernel mode
  (it writes the new kernel through kseg0), with a block of what it had from the BIOS
  (`struct kreplace_handoff`: the video's, the SIF's registers, the path). The new one, entered
  in kernel mode, finds the block, puts in its own vectors and tables, and goes on in user
  mode as after its own restart. The block's layout is what two kernels must agree on.
- `make KBIOS_GONE=1` (a test build) also fills 0x1000-0x87FFF with break instructions, and
  `tests/ps2/k62_bios.c` reads it back: on a SCPH-70004 and a SCPH-90006 nothing was written
  there over a boot, the tests, IOP resets, the kernel's restart and kernels replacing each
  other.

Also version 31: the BIOS's memory is frames (2026-11-16, mx OS plan step 4a part 3;
`apps/mxos/ee/exec.c`'s `kvm_boot`, `src/vm/vm.c`'s `mx__vm_frame_reserve`). Where the kernel
has left the BIOS, the VM's frames are 0x8000-0x1EFFFFF: 991 of them, 31 more than from 1 MiB.
Two pieces stay out: the first 32 KiB (the vectors' page, and what is left of a frame) and
the frame at 0xF8000, where mxlink's IOP module writes the host's commands (0xFF800). Where
the BIOS stays (PCSX2, a console the kernel does not know) the frames start at 1 MiB as
before. `tests/ps2/k63_frames.c` writes and reads every free frame and counts those below 1
MiB it had.

Also version 31: the kernel's directory and the card driver (`apps/mxos/ee/main.c`). The IOP
modules, the settings and the default shell come from the directory of the path the kernel was
started by (`argv[0]` up to its last `/` or `:`, at its first start: `module_dir`, kept through
restarts), so a kernel on `mc1:` or in a one-shot's directory uses its own files; a kernel
started from `host:` (tried from the PC: not whatever files are in the PC's directory), or one
without `chime.irx` there, from `MODULE_DIR` (`mc0:/MXOS/`). The card
driver is `mxmc.irx` (`iop/sio/mxmc`, a fork of ps2sdk's XMCMAN: the same device `mc` and
export table `mcman` 2.3, so MCSERV binds to it) with 16 open files in place of mcman's 3: the
VM keeps each running program's ELF open, so the service and the shell took 2 of the 3 and
everything else on the card fought for the last one (`docs/mxmc-plan.md`).

Version 30 (2026-11-16): the network (`apps/mxos/ee/net.c`, `<mx/net.h>`'s `mx_net_status`,
`common/include/mxnet.h`). mxsock watches mxnet (its new exports `mxnet_status`,
`mxnet_config`) twice a second and writes a `struct mxnet_status` to the kernel by DMA when the
link, the address, the DNS server or the DHCP state changed (`MXSOCK_NET_WATCH`, a counter one
more). The vblank hook sees the counter change; the command thread then sends `MX_EVENT_NET`
to the processes with events, adds a notification (`MX_NOTIFY_NET`: "Network connected" with
the address, "Network disconnected") and writes the screen's network line. What was last told
is kept across the kernel's restarts, so a program's end brings no news. `mx_net_status` gives
link, speed, duplex, MAC, address, mask, gateway, DNS, the DHCP state and the lease left. The
set-up comes from the settings: `net.mode` `dhcp`, or a fixed address (`net.ip`, `net.mask`,
`net.gw`, which mxnet starts with, and `net.dns`). The kernel applies them at its start when
the stack differs, and a second after the last change of a `net.*` key
(`MXSOCK_NET_CONFIG`). The host link's sockets are bound to any address, so it follows a new
one. Without the new mxsock (an older card) the network is not watched: `mx_net_status` says
`up` 0. `tests/ps2/k57_net.c`.
The kernel's VM keeps a fault log of one program (`docs/vm.md`, "The fault log"):
`mx_vm_trace_restart`, `mx_vm_trace_mark` and `mx_vm_trace_save` record its page faults (the
page, the EPC, the time, what it took) instead of returning -1. `tests/ps2/k58_faults.c`.

Version 29 (2026-11-15): the time (`apps/mxos/ee/time.c`, `<mx/time.h>`). At each start the
kernel reads the PS2's clock (battery backed, in the CD/DVD controller) through mxio
(`MXIO_CLOCK`: cdvdman's `sceCdReadClock`, found by its export table as bdm's): eight bytes, a
status and BCD seconds .. year in Japan's time, which every PS2 keeps (the SDK's "Overview -
Other": JST = UTC + 9 h). A status other than 0 (the clock stopped, its battery, a command
error: the clock reads 2000-01-02 then) leaves the time unknown: `mx_time` 0. Else the kernel
keeps UTC at its second 0 and counts on with its seconds (dev.c's `kseconds`). `mx_time_set`
(the shell only, 2000..2099) writes the clock too (`MXIO_CLOCK_SET`, `sceCdWriteClock`).
`mx_gmtime` and `mx_localtime` are libmxk's (no kernel call): the local time is UTC moved by
the settings `time.zone` (minutes east of UTC) and `time.dst` (`on`: an hour more); the OSD's
own zone is not read (a BIOS call). On PCSX2 the kernel has no mxio: the time is unknown.
`tests/ps2/k56_time.c`: the date arithmetic on known days, and on the PS2 the time read and
counting on.

Version 28 (2026-11-15): the controllers for the shell (`apps/mxos/ee/pad.c`, `<mx/pad.h>`,
`common/include/mxpad.h`). `mx_pad_info` gives every port's pad (`MX_PAD_PORTS`: the two
controller ports and four USB or Bluetooth pads; -1 past them): kind, link (port, USB,
Bluetooth), battery, cable, charging, mxpad's slot, the light bar's colour and an id, mxpad's
`mxpad_id` of its Bluetooth address (FNV-1a), which mxpad.irx now writes in the record (the
field was `reserved2`) with the new flags `MXPAD_CHARGING` (DS4: on the cable below level 11,
DualSense: state 1, DS3: 0xEE) and `MXPAD_BT`. `mx_pad_gen` counts every change of these, of a
SIO2 port and of the pairing (`MX_EVENT_PAD`'s status). The shell only: `mx_pad_disconnect`
(`MXPAD_DISCONNECT`: the Bluetooth link ends, the pad turns itself off), `mx_pad_pair`
(`MXPAD_PAIR`, 0 seconds stops) and `mx_pad_forget` (`MXPAD_FORGET`: out of `BTPADS.DAT`, its link
ended); for every program `mx_pad_pair_state` (`MXPAD_PAIR_STATE`: idle, searching,
connecting, done, not found, failed, no dongle; a pairing by USB cable is told as done too) and
`mx_pad_paired` (`MXPAD_PAIRED`). The command thread (`pad_watch`) watches the pairing while it
runs and for 10 s after a USB pad came (notifications "Controller paired", "Pairing failed"),
sends each slot's light bar (`MXPAD_COLOURS`: `pad.<id>.colour`, else the player's colour, by
`pad.brightness`, 25 by default: mxpad's colours before) and turns off a Bluetooth pad on battery
whose record did not change for `pad.idle_off` minutes. Its calls and the shell's go through two
SIF clients of their own (`cmd_rpc`, `shell_rpc`), so they never meet a program's rumble on
the first. mxpad.irx keeps version 1: a kernel before 28 takes it. `tests/ps2/k53_pads.c` prints
it all and each `MX_EVENT_PAD` while pads are plugged in and out (by hand).

Version 27 (2026-11-15): the settings store (`apps/mxos/ee/settings.c`, `<mx/setting.h>`).
One store of the system's settings, keys with dots and text values (key up to 31 bytes, value up
to 63; 64 of them), in place of the card's separate files: `mc0:/MXOS/SETTINGS.DAT` and
`SETTINGS.NEW`, a text file ("MXOS settings 1", "seq=N", "key=value" lines, "end") written as
two copies in turn: a write goes to the one that is not the newest, so a power cut while
writing leaves the other complete, and at the boot the complete one with the higher seq wins.
The kernel reads it at its first start (with the loader's IOP, which reads the card) and keeps
it in `.padarea`, with a magic and its directory, through its restarts (its `.bss` is cleared
then; `.padarea` is read-only through the TLB, so the store and its file buffer are used through
the uncached alias); the command thread writes it 3 s after
the last change (`settings_tick`, from `dev_now`); a failed open, write or close is logged with
the open's and the write's results and tried again 2 s later when the card driver had no file
free (-24), else 30 s later (the card may be out). Without a store the old files are imported
once: `IPCONFIG.DAT` (`net.ip`, `net.mask`, `net.gw`), `SWAP.CFG` (`swap.path`, `swap.size`),
`SHELL.CFG` (`shell.path`), `SERVICES.CFG` (`service`); they are not read again after that. The
kernel's keys: those, `storage.mx4sio` (`on`: `mx4sio_bd` loaded at the boot) and `pad.swap_ports`,
`pad.swap_xo` (the home menu's toggles, saved). `mx_setting_get`, `mx_setting_list` (the changes
after a seq, in their order; a removed key is listed with "") and `mx_setting_seq` for every
program; `mx_setting_set` for the shell only (-1 for another). A change posts
`MX_EVENT_SETTING` (status: the store's seq) to every process with events. On PCSX2 the store
is `host:settings.dat`/`host:settings.new`, and a test run's `host:swap.txt`, `host:shell.txt`,
`host:services.txt` are imported at every boot (a missing one removes its key).
`tests/ps2/k52_settings.c`: as the shell `set` (get, set, remove, the event, the limits) and,
after a new boot, `check` (read back from the file); `noshell` (set refused).

Version 26 (mx OS plan step 4e, 2026-11-15): the shell is a role of the card's configuration.
`mc0:/MXOS/SHELL.CFG` (on PCSX2 `host:shell.txt`) names its path (`shell_path`, `ee/main.c`; since
version 27 the setting `shell.path`); without it the shell is `MXSHELL.ELF` in the kernel's directory, as
before. The process table keeps the roles (`ptab_shell`, `ptab_service`) and `mx_proc_list` gives
them in `flags` (`MX_PROC_SHELL`, `MX_PROC_SERVICE`; the field was padding, 0 before). mxlink's app
finds the shell by that flag on this kernel, by the file's name on an older one. `k50_home` checks
the flags; on PCSX2 `host:shell.txt` names the shell.

Also version 26: the VM is the kernel's, made at its boot (`kvm_boot`, `ee/exec.c`, after the
IOP's modules are loaded: they were read to 16 MiB, the programs' memory) rather than by the first
program with VM and handed on at its end. maxsdk's `mx__vm_boot` (`ee/src/vm/vm.c`): the frames are
the memory from 1 MiB to 64 KiB below the top, where the VM's tables are (the physical allocator
given that range first); `space0` is the kernel's, of one page and no windows, and the spaces of
programs get the windows (`new_nmap`, `new_nheap`) and, without a size of their own, 128 MiB with
a swap file or the frames without. The writer and the pager are threads of the kernel at priority
1, made there (`writer_make`, `pager_make`): the writer keeps no frames clean until a program asks
(`mx_vm_config.clean_frames`, `mx__vm_writer`). From then on the kernel is as while a program
with VM waited (phase 3a), for every program (`VM_KEPT`): each program's core is pinned frames
(`core_place`), its VM a space (`kvm_init`), and its end gives both back (`kvm_front_gone`, the
kernel's space in front again); the VM's vblank hook stays (`end_cleanup`). The swap file is
MXOS's only (`SWAP.CFG`, since version 27 `swap.path`; opened at the boot or at the first program with VM once the card reads):
a program's or a launch's own is not used. A program without VM that would take all memory is
refused ("takes all memory"); one whose core finds no frames ("no memory for its core"). A page
fault on a thread whose stack is in the kernel's memory (from 0x80000) is served too (`to_thread`:
the boot's test of the TLB). The kernel's restart turns the VM off first (`kvm_restart`: the
BIOS's TLB and handlers back, the swap file closed by the boot's IOP cleanup), a kernel that
replaces this one too (`kvm_off`); the chime loaded at a program's end without mxsnd goes through
pinned frames, not 16 MiB. Without the VM (PCSX2's recompiler raises no TLB misses) programs are
placed as before. On PCSX2 the swap file's setting is `host:swap.txt`.

Also version 26: devices, notifications and the swap file (`ee/dev.c`, `ee/swap.c`; `<mx/dev.h>`,
`<mx/notify.h>`). The kernel loads the storage drivers after mxpad (`bdm`, `bdmfs_fatfs`,
`usbmass_bd`, `mxata`; `mx4sio_bd` not: it keeps probing slot 2 and mcman then finds no memory
card there). mxio watches the devices (`MXIO_WATCH`, `common/include/mxio.h`): bdm's callback
(`bdm_RegisterCallback`, found by its export table: bdm loads after mxio) for the drives, and
each card every second: through mxmc's library `mxmc` (`common/include/mxmc.h`, found at run
time: `mxmc_card`, the card's check under mxmc's lock without taking one of its files, with its
size and free space, which mxmc counts again only after a page was written or erased), else
(the ROM's or ps2sdk's mcman, as in an installer's IOP) by `dopen` of `mc0:/` and `mc1:/`, where a
card counts as gone only on "no card" or "not formatted" (-ENXIO/-EFORMAT; -EMFILE keeps the
slot as it was). A card's free space changes the list without a new counter, as the files open
on a device do; it writes its list with a counter by SIF DMA
into `.padarea` (above the kernel's code, as mxpad's), and a device that went stays listed, not
present, for one change. The kernel's vblank hook (`dev_vblank`) sees the counter change and
wakes the command thread (`dev_now`), which compares the list with the names it knew (the first
list after the boot is taken as it is), adds notifications, tells swap.c and posts
`MX_EVENT_DEVICE` to every process with events (`kmsg_post_all`: not to one that waits or is
paused, and not while one of the same type is still queued for it). Twice a second the command
thread also looks at the controllers (`pad_watch`, `ee/pad.c`): mxpad's records (kind, cable,
battery) and the SIO2 ports while a program has them open; a change is a notification and
`MX_EVENT_PAD`. A SIO2 port's change counts when two checks in a row see it, and for 3 s after a
switch between processes or the ports opened or closed the state is taken without a word
(`pad_watch_quiet`): a port reads as empty for a moment then, which looked like a pad that went
and came at every home press. Events are hints in the order the kernel posts them, not tied to a
waiter: a process's `MX_EVENT_END` of a killed child can come before the thread that waits for
it is ready, so a thread that drains the event port keeps END and HOME per process. The notifications are a ring of 32 in `.padarea`, written through the uncached
alias (programs may not write there), each with an id, a seq that changes when it changes (an
urgent one resolved, `knotify_resolve`), the seconds since the boot, kind, urgent, action
(`MX_NOTIFY_ACTION_*`: the shell's page to open), the source (the program's file name, "" for
the kernel), title and text. swap.c attaches the swap file from the setting (`SWAP.CFG`) when its
device is there (`kswap_device`), opening it first to know whether it is made now, and on the
device's removal calls maxsdk's `mx__vm_swap_gone`: a resident page whose copy was in the file is
dirty again, one that was only there `PG_LOST` (its fault ends the program). `mx_swap_set` (the
shell only) writes the setting and attaches at once; while the old file holds pages
(`mx__vm_swap_detach` refuses) the new place counts from the next start. `tests/ps2/k51_notify.c`
prints the list, the swap state and every event and notification while devices and controllers
are plugged in and out (by hand).

Version 25 (mx OS plan step 4d, 2026-11-15): `proc_shell`, and the home button brings the shell in
front. When the shell (the program the kernel started as its shell, `exec_shell_next`) waits for the
program in front or runs beside it (`mx_proc_spawn`), `home_run` (`ee/home.c`, at the program's
frame boundary, where the kernel's menu would open) asks the command thread for the shell in front
(`kproc_front_home`): the program is paused as by `proc_front`, and once the switch is done the
shell gets `MX_EVENT_HOME` with the paused process (sent after the switch: a shell beside that got
it before found the game still in front). Until the home button is let go the shell sees no buttons
(`home_hold`, in its kernel state, after the switch); with the shell in front the home button is the
shell's, without the kernel's menu, and `mx_pad_buttons` does not end the shell at it (`proc_shell`:
1 for the shell, in front or beside). `proc_kill` of a process that waits or is paused
(`kproc_front_kill`) brings it in front and ends it there; the caller is paused meanwhile and in
front again after that end, and a parent gets `MX_EVENT_END` also for a child that came in front by
such a switch. Front requests are numbered (`front_request`), and after each switch every waiting
caller is woken and looks whether its own is done: with one caller to wake, a request made while
another's caller was paused took its wake-up, and that program never came back. A switch closes no
pad ports, so the one brought back reads its pads as it left them; only when a program's end closed
the ports meanwhile (`pads_close`, counted) does it open them again at its next `mx_pad_init`. Its
video mode is set again at the switch (`home_video_again`, pulled forward from step 5): the kernel
keeps the timing a process last set (`kvideo_crtc_set` copies it, `home_crtc_seen`) and its display
registers with the rest of its kernel state, and writes both when it is in front again; so a shell
in 1080i and a game in 576i each get their own mode at home and resume. Not yet at a child's end
(the parent sets its mode itself, as before), and the GS memory is not kept (step 5). Without a
shell that waits (a host's program, MXSHELL's `mx_loader_launch` today) the kernel's menu opens as
before. `tests/ps2/k50_home.c` is the shell and the game in one: on PCSX2 the emulator kernel built
with `MODULE_DIR=host:` starts `host:MXSHELL.ELF` from `host:mxload.txt` as its shell (`poll`:
without the frame pipeline, which stalls on PCSX2). The game presses the home button with a line
`@@PAD <hex buttons> <ms>`, which only a test build of the kernel turns into a pad's buttons (a hook
kept out of the kernel); on the PS2 such a build also starts the test as its shell, and it passes
there with the frame pipeline, about 5 s a step to watch the mode switches on a TV.

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
and after a restart, the kernel starts the service in `mc0:/MXOS/SERVICES.CFG` (since version 27 the
setting `service`; a path and its arguments; on PCSX2 `host:services.txt`, the runner's `MXSERVICE`) in place of the shell.
With a service beside, the kernel prints no test end marker: it is the service's (2e-4:
mxlink's, for the host's runs).
Since 2026-11-12 the parked programs are a stack of two (`exec.c`'s `parks`, `end.c`'s
`parked_at`, `kproc.c`'s `sides`): above a service the program in front may start another
beside it (`launch_beside`, `proc_spawn`), so both run beside the new one in front, each in its
own address space (its cores' TLB entries kept after those of the one below, a switch between
two of them goes through the front's state). Its end unparks it (it is in front again, the
service beside); the service's `MX_EVENT_END` is for the program it started. One that waits
(`launch` without beside) above a service stays too when it has VM (its core pinned frames of
the VM; before 2026-11-12 it took the restart's way and the other ran in its place, so on the
PS2, where mxlink's app is always the service, `mx_loader_launch` never waited: `k29_stay`,
`vm26_launch` and `vm38_vmbeside` passed there falsely, their child ending with 0; it ends with
7 now); one without VM still takes the restart's way (it crashed at once: its stack in the next
one's core). A program that
takes all memory does not start while two are parked (the service's core is not its to give).
Before, with mxlink's app as the service on the card, such a start failed (-1): `k31`-`k36` and
`k42` failed on the PS2.
Since 2026-11-12 (mx OS plan, phase 3a) a program with VM may also be parked: its VM stays on,
and the next program's `mx_vm_init` gives it an address space of its own (vm.c's
`mx__vm_space_new`) on the same frames and swap file, one LRU over both (the space's size:
the program's `mx_vm_config.size`, 128 MiB if it does not say, when it has a swap file; without
one the first space's, which is no more than the frames when a service without a swap file
turned the VM on. Before 2026-11-13 every space had the first one's size: 30 MiB under the
host link's service, and a game's 16 MiB `malloc` failed); its core is a run of
those frames (MiB-aligned, pinned), and its end gives back both (`exec.c`'s `kvm_init`,
`kvm_shutdown`, `kvm_front_gone`). `kproc.c` switches the space with the ASID (the VM's TLB
entries go at each switch). The VM's state in the kernel (resident.ld's `__vm_*`: vm.c,
image.c) is not set back, saved or loaded with the rest of a program's while such a program is
parked (`end.c`). The VM's tables are read through `MX_PHYS_WIN` (a global entry), so every
process's threads and the kernel may use them. `tests/ps2/vm38_vmbeside.c`. A program's
`mx_vm_config.frames` is the most frames its space holds then (`mx__vm_space_limit`: above it,
one of its own pages goes; otherwise one LRU over all), and its swap file is opened for all
spaces if the VM has none yet (`mx__vm_swap_open`: a service without one turned it on). Such a
file is the program's own (not MXOS's `SWAP.CFG`): when its space is freed and no other space
has a page in it, the VM closes it and has none again (since 2026-11-12: on the PS2 a `host:`
file goes with the host's run, and the next run's descriptors took its number, so the next
program wrote its pages into its own ELF). At a program's end the kernel closes the program's
descriptors but the VM's (`mx__vm_uses_fd`: the swap file, the ELF and mapped files of the
spaces that go on), FILEIO's and mxio's (sif.c's `mx__fio_cleanup`). When the VM keeps none,
that is every FILEIO number 3..31 and one `MXIO_CLOSE_ALL`. When it keeps files, only the
FILEIO descriptors the EE opened itself (sif.c keeps a mask of them) and mxio's slots one by
one but the VM's: FILEIO's numbers are ioman's, and mxio holds ioman handles of its own for
its files on a driver (`usb0:`, `mc0:`), so closing every number closed the VM's files behind
mxio's back (`tests/ps2/vm43_keep.c` for `host:` files, `vm44_keepdev.c` for a file on the
card). A space closes its own files when it is freed. A
program without VM that takes all memory (no core heap) does not start while a program with VM
is parked: `k40_frame` and `k41_present` have a core heap for that.

One swap file for MXOS (2026-11-12, plan phase 3c; `apps/mxos/ee/main.c`'s `kswap_path`):
`mc0:/MXOS/SWAP.CFG` (on PCSX2 `host:swap.txt`) names it and, optionally, its size in MiB (64 by
default); since version 27 the settings `swap.path` and `swap.size`. A program whose launch gave no swap file gets it
(`largs.swap`, in place of its `mx_vm_config.swap`), and the kernel tells vm.c the size before
the program's `mx_vm_init` (`mx__vm_swap_size`). The VM's slots are a bitmap up to that size
(at most 256 MiB): a space's go back when it is freed, so the file does not grow with each
program; a page that finds none ends the program ("the swap file is full", through
`mx__crash_end`, its lock given up). Without the file the programs' own swap files are used
as before. `tests/ps2/vm39_swapcfg.c` (with `host:swap.txt` of 4 MiB it passes on that file,
with 1 MiB it ends at page 32).

Version 19: timers are the kernel's service (`<mx/timer.h>`, `src/irq/timer.c`; the kernel
builds its maxsdk with `MX_SOFT_TIMER=1`). Software timers (`mx_timer_add`, once or every
period, and `mx_sleep_us`'s sleepers) are slots of one table, each due at a COP0 Count, on EE
timer 1, which no program sees: it is set, one-shot, to the earliest, and its interrupt runs
what is due. A callback runs in its process's address space (`mx__timer_enter`, `kproc.c`), so
it and its argument are in core. The timer's handler itself runs in the front's, so this is a
switch inside a switch: each one returns to the space it was entered from (at most 4 deep). The other EE timer, 0, is lent out whole (`mx_timer_claim`,
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
`kreplace.S`): an ELF whose segments lie in the kernel's memory, over the running kernel (a new
`MXKERNEL.ELF`, `ps2client execee host:MXKERNEL.ELF`) is read to 0x0100_0000, a table of its
segments and a trampoline go to 0x00F0_0000, and ExecPS2 starts the trampoline, which copies the
segments over the old kernel, clears their bss, writes the data cache back, drops the
instruction cache and jumps to the new kernel's entry with ExecPS2's arguments. It boots as after
a restart (the IOP reset, the services and the shell started again). Once the
kernel has left the BIOS (below) it starts the trampoline itself instead of ExecPS2.
While a program with VM is parked (a service: so at present always on the PS2) the kernel
first restarts itself without the services and takes the new kernel then (since 2026-11-13):
the replace starts with `tlb_restore`, which takes away the `MX_PHYS_WIN` entry the VM's
tables are read through, and 0x0100_0000 and 0x00F0_0000 are frames of the VM (before, the
VM's vblank handler missed the TLB and the old kernel went on with a crash on its screen).

Other consoles (2026-11-13). The kernel reads and writes BIOS data by address (the exception
and interrupt tables, the threads, the SIF registers), and another ROM has them elsewhere:
`kirq.c`'s `bios_find` takes the tables' place from the BIOS's own vectors and applies the
difference to all of them (`docs/bios.md`; ROM 2.00: 0, ROM 2.20: 0x5C0). Vectors that are not
the BIOS's code leave the difference at 0. Nothing may be printed before the interrupt tables
are found: a line on the console starts the SIF, which hung the start on ROM 2.20; what
`bios_find` and `kvideo_init` found is said after the console is up (`kirq_report`,
`kvideo_report`). A start that shows nothing can be followed by its colours: `make KTRACE=1`
sets the screen's border colour at each step of the start (`ee/main.c`).

A report of a hang (2026-11-13; `ee/main.c`'s `hang_threads`): at the host's reset while a
program runs, the kernel notes every thread (its process, state, priority, epc, ra, sp, and the semaphore it waits on), the
VM's last exception (`mx__vm_dbg_last`: the address, the EPC, how many times in a row, the
page's state), COP0's BadVAddr, EntryHi and EPC and the TLB's entries, in memory that the
restart keeps, and prints them at the next program from `host:` (`hang_log`); the SIF's own
"stuck" lines go there too. With them the SIF's state (`mx__sif_dbg_line`: the last 32
packets received, numbered; each thread asleep for an answer with its client, function,
packet and the number of packets received when it began: so an answer that never came and
one that came without waking its thread can be told apart), the VM's pages whose TLB entry
was not their own (`vm.c`'s `stale`; the cause was a return without SYNC.L, vm.md's facts: put right when seen, counted, also in the VM's lines at
a program's end), and a wait asked for inside an interrupt handler. At every
program from `host:` the log also has the last program's crash, if it crashed (its report
to the host can be lost), and the VM's pool: frames, free, pinned. So after a program hangs: `ps2client reset`, then any run shows
where it stood.

The kernel runs MXOS programs only (2026-11-11, mx OS plan step 2h-2; `apps/mxos/ee/exec.c`):
an ELF without the MXK note of version 10 or later (a standalone ELF on `libmx`, ps2sdk's, an
older `libmxk`) and a loader (an ELF below 1 MiB that is not a kernel replacing this one, such as
ps2link) are refused with a message; mxboot, a legacy loader or PCSX2's `-elf` starts those. Gone
with them: the kernel's restart with the BIOS's threads for such a program (`bios_mode`), the
loader's start by ExecPS2, and the
replaced BIOS `Exit` (syscall 4) for programs that did not return through the kernel.

The BIOS syscalls a program still makes are counted (2026-11-11, mx OS plan step 2h-3;
`apps/mxos/ee/ksys.S`, `kirq.c`): the kernel points the BIOS's exception table entry for
syscalls (0x8001_5360, `docs/bios.md`) at a stub that counts, by `v1 & 0xFF` (an i-call's negative
number at 0x81-0xFF), the syscalls of the program the kernel started last (the current ASID its,
EPC in its own code: not in the kernel's MiB, not the BIOS's in kseg0/kseg1), then goes on
into the BIOS's dispatcher; the table is the BIOS's again whenever the interrupt table is
(`kirq_off`). At the program's end the log has `BIOS syscalls: N, by number:` and the numbers
with their counts (calls made through the kernel's services are the kernel's and not counted).
After them, to the host, the calls that waited for the IOP while the program ran (any
process's; since 2026-11-13): a line for each server and function, `IOP server <id> function
<n>: <calls> calls, <ms> ms, the longest <us> us` (the server's id as bound: `80000001` the
file server, `4d58494f` maxsdk's file module, `4d58534e` its sound module; the first 19
pairs, the rest in one line with server 0). It tells a frame's calls from loading and
swapping: a Quake 3 demo's waits were page transfers and sound loads, none of them a pad or
sound call of each frame.

`libmxk` calls the kernel only (2026-11-11, plan step 2h-3b): built with `MX_KERNEL`, its
thread, semaphore and interrupt handler calls (`ksys.c`) have no BIOS path, its start-up has no
`SetupThread`/`SetupHeap` (a program needs the kernel's threads, version 10, and stops with a
message without them: the BIOS's `Exit`, the one BIOS call it keeps), its SIF DMA calls are
`kstubs.c`'s (the kernel's services come first; one that is reached anyway ends the program
with a message), and `gs.c` never calls `SetGsCrt`: the kernel's `crtc_set` does, for a mode the
BIOS has, where its own timing does not know the console (PCSX2; `apps/mxos/ee/kvideo.c`). A
`libmxk` program built so needs a kernel with this `crtc_set` for a video mode on PCSX2. `libmx`
keeps the BIOS's paths.

Since 2026-11-12 (plan step 2h-3c) `libmxk` has no BIOS stubs at all (`syscalls.S` is `libmx`'s
and the kernel's), and its one syscall is the `Exit` of `mx__no_kernel_exit` (`start.c`). The
build checks `libmxk.a` and every MXOS program it links (`tools/mxos-bios-check.sh`, in
`$(call link)` of `mk/maxsdk.mk`): a syscall instruction anywhere else fails the link and
removes the ELF. `<mx/bios.h>` is not for MXOS programs: it stops with an `#error` unless
`MX_BIOS_CALLS` is defined (`mk/maxsdk.mk` does for `MX_TARGET=standalone`, maxsdk's Makefile
for its library), and no public header includes it; the kernel's thread table, which
`<mx/kernel.h>` needs, is `<mx/kthreads.h>`. A program's `mx_video_board_offset` (the BIOS's
GetGsDxDyOffset) asks the kernel (version 23, `board_offset`). The BIOS's call reads the GS's
revision and BIOS memory filled at its boot (0x8001_5960, 0x8002_2588; a jump table by mode
code 2 to 0x73), so the kernel asks it once, at its start, for every code (where the ROM has the
call: ROMVER after 2001-06-08), keeps the answers in `.kept` (`kvideo.c`'s `dxdy`) and answers
from them, without the BIOS (since 2026-11-12; the same values as the BIOS's on PCSX2:
`examples/display`).

The kernel's own syscall entry (2026-11-12, plan step 2h-4a; `apps/mxos/ee/ksys.S`, `kirq.c`):
the stub in the exception table's syscall entry also runs the kernel's own numbers (the
scheduler's switch 0x3B, the poke 0x69, the VM's resume 0x5B: `ksys_set`, a table by |number|)
the way the BIOS's dispatcher runs a SetSyscall handler (the same save area, kernel stack and
frame, `docs/bios.md`), and sends the others on to the BIOS's. maxsdk sets its syscall and
exception handlers through `src/base/excset.c`: the kernel's (`mx__kernel_syscall_set`,
`mx__kernel_handler_set`: the exception table written by the kernel, a handler below 32 MiB
through kseg0) once its entry is in, else the BIOS's calls. SetSyscall is left only as the
poke's bootstrap at each start and before a restart. The debug output at a program's end has the
kernel's own BIOS syscalls since its boot (`the kernel's own BIOS syscalls since its boot`).
The SIF's software registers (SifGetReg/SifSetReg with bit 31: the IOP's buffer address, the
RPC's state) are the kernel's while it has the SIF (2h-4b, `src/base/sifdma.c`): copied from the
BIOS's table (0x8002_12C0, `docs/bios.md`) at `mx__sif_dma_take` and written back at
`mx__sif_dma_give`, before a restart, so the BIOS and the next start find them.

The level 1 vectors are the kernel's (2026-11-12, plan step 2h-4d-1; `apps/mxos/ee/kvec.S`,
`kirq.c`): `kirq_on` copies its own code over the BIOS's at the TLB refill (0x8000_0000), common
(0x8000_0180) and interrupt (0x8000_0200) vectors. They do what the BIOS's do, through the
kernel's tables (`kvec_exc_table` by ExcCode, `kvec_irq_table` by the highest pending line),
which `kirq_on` fills from the BIOS's (0x8001_5340, 0x8001_5380) before pointing the syscall
entry and the INTC and DMAC lines at the kernel's. The BIOS's tables are no longer written:
`mx__kernel_handler_set` writes the kernel's. `kirq_off` (before a restart or a replace) puts
the BIOS's vector code back (kept in `.kept`), so the BIOS's calls of the next start work as
before. The poke writes the vectors in one syscall each, writes the lines back from the data
cache and invalidates the whole instruction cache and BTAC by index: the instruction cache's
CACHE operations must not get an address with bit 31 set (the hardware restrictions), so not
the vector's kseg0 address. The poke's `SetSyscall` is now only at `kirq_on` (its bootstrap).

Since plan step 2h-4d-2 the interrupt vector and the syscall entry keep the interrupted (or
calling) thread's sp, ra and at in the kernel's save area (`kvec_save`, not the BIOS's at
0x8000_10C0), and the kernel's own positive syscalls run on its own stack (`ksys_stack`, not the
BIOS's at 0x8001_8E70), with the BIOS's frame (ra, the caller's sp, EPC + 4). The interrupt
lines that are not the kernel's but have a BIOS handler (only 7, the COP0 timer) go through
`kvec_irq_bios`, which copies the three registers into the BIOS's save area for that handler and
counts them (`(N others to the BIOS)` in the log at a program's end; 0 on PCSX2). Exception codes
0, 11 (coprocessor unusable) and 14, 15, which the BIOS leaves to its default handler (a message
and a break), go to the kernel's crash report. The BIOS's save area and stack are still used by
what goes on into the BIOS: its syscalls (`ksys_orig`) and its INTC and DMAC handlers before the
kernel's threads run.

Version 22: the video timing without the BIOS (plan step 2g; `apps/mxos/ee/kvideo.c`), and
`iop_write` (below). maxsdk
computes the CRTC registers (SMODE1 with the PLL, SYNCH1, SYNCH2, SYNCV, SRFSH, SMODE2) from a
description of the line and the frame (`<mx/crtc.h>`, `ee/src/gs/crtc.c`); the descriptions of
the BIOS's modes give the values its SetGsCrt writes, all 21 of them (`tests/host/test_crtc.c`),
and new ones are made the same way (`mx_video_720p50`, `mx_video_1080i50`, `mx_video_1080p30`,
or any `struct mx_crtc` in `mx_video_mode.crtc`). `mx_video_set` hands the description to the
kernel (`crtc_set`), which writes it as SetGsCrt does on the console it finds (two known:
ROM 2.00's, a SCPH-70004, and since 2026-11-13 ROM 2.20's, a SCPH-90006, which has no clock
switch and its encoder on another bus, `docs/bios.md`; told apart by the BIOS's place of its
data, two instructions of its SetGsCrt and its variables; any other console: the BIOS's
SetGsCrt). 576p (`mx_video_576p`, 2026-11-13) is ROM 2.20's BIOS's mode, written by the
kernel on both consoles. For ROM 2.00's: the HD clock switch (0xBF80146C bit 3,
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
SIF0 has two handlers in the kernel, sif.c's (the packets in its buffer) and the host link's
(`apps/mxos/ee/cmd.c`, the packets at `0xFF800`). Each starts the channel again only after a packet
it took itself, and sif.c reads its buffer only while the channel is stopped: started before the
other handler had copied its packet, the channel let the IOP's next packet land on top of it (on
the PS2 an RPC's answer was lost so, at a program's start or end with a service beside; PCSX2's
kernel has no host link handler).
`libmxk`'s `mx_iop_write` goes through the kernel (version 22: `iop_write`, a physical source):
a program's own SifSetDma would meet the kernel's ring. A SIF wait of over 3 s shows the ring's
state on the kernel's screen, and a crash of the kernel shows there at once, before the report
goes out through the SIF.

Two things a process beside showed: a program's end waits only for the IOP calls of its own
threads, those that are gone (`mx__sif_calls_finish` keeps the calls and packets of threads
still alive: a service's tty write was waited for until it gave up, and its packet was
freed). An interrupt handler whose code is the kernel's (in its memory, its `irq.c`: the SIF
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
`mx_loader_launch` and the host's reset still take the restart. Changed means since the last
start: a program that waits (`mx_loader_launch`) makes the modules it loaded (the shell's USB
drivers) the IOP as it was for the other's end (since 2026-11-12; before, a game the shell
started from USB ended in the restart, and the shell and the service with it) (programs not on `libmxk`, and
those before version 8, which did not tell the kernel their interrupt slots, took it too until
2h-2 refused them):

Each such program ends in the kernel's restart. Since 2026-11-12 (plan step 2h-4e-2) the first
start's restart, too, is the kernel's own: mxboot (or PCSX2) started the kernel by `ExecPS2`, so
there is nothing of a loader's to shed (before, it restarted by `ExecPS2` again; tested on
PCSX2 with the PS2 build from a card image: `make READY_END=1`, the runner's `PCSX2_MC` and
`PCSX2_NET=sockets`). Every restart jumps to `mxlink_start` itself (2026-11-12, plan step 2h-4c; `apps/mxos/ee/main.c`'s `restart`),
in user mode with interrupts off on the BIOS thread it runs on: the kernel's threads,
semaphores and interrupt handlers are its own, so there is nothing of the BIOS's to drop.
Since plan step 2h-4e-1 that restart keeps the kernel's vectors and tables (`kirq_keep`, not
`kirq_off`; the tables, the syscall handlers and their flags in `.kept`), with every INTC cause
and DMAC channel masked off until the kernel's handlers turn theirs on again (the BIOS has no
handler of the kernel's vblank). The start-up then sets its exception and syscall handlers in
the kernel's tables and skips the BIOS's SetupThread and SetupHeap (`start.c`'s weak
`mx__kernel_kept`): the restart calls no BIOS, and what the kernel still calls is counted since
its boot (on PCSX2 only SetGsCrt). The first start, too, turns the vectors on in its start-up
(`mx__kernel_early`, before `mx_exception_init`) and reads its configuration after `ksched_init`:
its only BIOS calls before the vectors are SetupThread (the boot path) and the poke's SetSyscall. Either
way the kernel is not loaded again: `.bss`
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
kernel writes the next one's entries again (`tlb_emu_asid`). Since 2026-11-12 the kernel
measures this at its start (`tlb.c`'s `tlb_ignores_asid`: two entries at one address under two
ASIDs, read under the first), so the PS2 build does the same when it runs on PCSX2: from a
card image (`make READY_END=1`; the runner's `PCSX2_MC`, `PCSX2_NET=sockets`) it runs
`vm38_vmbeside` whole, programs beside each other included.

## Crashes

The kernel's exception vector stays installed while a program runs. It reports what the
VM cannot handle: registers, stack and the `trace:` line go to the host, on the kernel's
stack. A crash inside a file call does not block the report: the file lock is given up.
The program ends with status 255 and the kernel restarts: after the report, exception.c calls
the kernel's `mx__crash_end`, which takes the program's way back (`mxlink_return`) while a
program runs (before 2026-11-12 the report went on into the kernel's own `mx_exit`, which has
no way back when PCSX2 started the kernel: the BIOS's `Exit`, a reset, and no status).

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
