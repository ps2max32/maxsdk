# mx OS: the plan towards programs in windows

Where mx OS goes from "one program at a time" ([mx-os.md](mx-os.md)), in steps that each run on
the PS2 on their own. Chosen 2026-11-07: programs in windows with address spaces of their own,
protected against accidents only (option B). Validating DMA chains in the kernel (option C) is
not done.

## Goal

- mxboot always starts the kernel; the kernel always starts the GUI (the shell grown into a
  window manager). The GUI owns the GS display and the video mode.
- Programs (Quake, mxlink's host link screen, the shell's own parts) run in address spaces of
  their own (ASID), each in a window: it draws into a render target the GUI gives it (for
  example 256 x 256) and the GUI shows it as a texture. A window has a menu: quit, the pad's
  settings, full screen.
- Full screen: the program gets all of GS memory and every video mode, as now.
- Programs on `libmx` and ps2sdk's run alone (full screen, the rest stopped), as now.

## What the hardware gives and does not give

| | |
|---|---|
| Address spaces | EntryHi's 8-bit ASID and the G bit (EE Core User's Manual, EntryHi): one virtual address on other physical memory per process, no TLB flush at a switch |
| No IOMMU | the DMAC uses physical addresses: a wrong tag reaches all memory. Address spaces protect against wild CPU stores, not against DMA |
| GS memory | no protection: a GIF packet can set FRAME or BITBLTBUF anywhere. Windows share it by agreement (libgl, maxsdk), not by force |
| Display | PMODE, DISPFB, DISPLAY, SMODE are on a page of their own (0x1200_0000): the kernel can keep them for itself |
| Overlay | the merge circuit blends read circuit 1 over circuit 2 with circuit 1's alpha (0x80 = 1.0, doubled and clamped) or PMODE.ALP (GS manual 5.2.3): a menu over a program's picture without drawing into it |

## Steps

### Step 1: the home menu (option A)

One program at a time, as now. Held together, Select + Start + L1 + R1 open the kernel's menu
over the program, which waits meanwhile: resume, quit, the pad's settings. Only for programs
on `libmxk` (the kernel's pad); the others keep the combination as quit.

- **1a. The pad is the kernel's.** `mx_pad_*` in `libmxk` call the kernel (table version 6,
  optional: a `libmxk` program still runs on a kernel of version 5, with its own pad code).
  The kernel opens the ports when a program asks, reads the records, applies the pad's
  settings (ports swapped, Cross and Circle swapped) and sees the combination. It closes the
  ports when the program ends, as now, so programs with their own pad code find them free.
  Done 2026-11-07: `tests/ps2/vm23_pad.c` passes on the PS2 on kernels 5 (its own pad) and 6
  (the kernel's), and `tests/ps2/pad.c` (libmx) after it finds the ports free.
- **1b. The display through the kernel.** maxsdk writes the display registers in one place
  (`apply`, `ee/src/gs/gs.c`); under a kernel of version 6 it hands them to the kernel, which
  writes them and keeps a copy, and keeps circuit 1 while its menu is up.
- **1c. The pause at a frame boundary.** The kernel asks; the program stops at its next
  `mx_frame_begin` with the GS idle, lends the kernel GS memory for the menu (free pages, else
  the texture cache's, which it then drops) and waits. A program that does not come within a
  second is not paused: the combination held for 2 s ends it, as now.
- **1d. The menu.** Drawn by the CPU, a band of lines at a time, and copied into the lent GS
  memory by IMAGE transfers through the GIF FIFO: only BITBLTBUF, TRXPOS, TRXREG and TRXDIR
  change, which every upload sets anew. It is shown on circuit 1 over the program's last
  picture on circuit 2. Circuit 1 covers the whole screen (320 x 224 CT32, 35 pages), so its
  alpha outside its rectangle does not matter.

  1b-1d written 2026-11-07 (kernel table version 7; `apps/mxos/ee/home.c`,
  `ee/src/gs/home.c`). `tests/ps2/vm24_home.c auto` passes on the PS2 (240 frames, their flips
  through the kernel). The menu works on the PS2 and a TV (`vm24_home` without `auto`, PAL
  640 x 512 interlaced: opened over the picture, the pad's settings, quit ending the program
  with status 0; 2026-11-07, seen by the user), and over Quake (menu, Cross and Circle
  swapped, resume, quit). Found with Quake: maxsdk had room for two functions at each vblank,
  both taken by the SIF waits and the VM, so the menu's was silently not added and it waited
  for good (now four, and a full table is refused aloud); audsrv played its ring over and over
  while the program waited (the stream is stopped for the menu now).

### Step 2: processes

**2a. A program ends without the restart** (`libmxk` programs; the others keep the ExecPS2
restart, they run alone). What ExecPS2 does now, the kernel does itself:
- *Threads, semaphores*: the kernel knows its own (the command thread, the VM's pager and
  writer, its semaphores); every other BIOS thread and semaphore is the program's
  (ReferThreadStatus / ReferSemaStatus over all ids), terminated and deleted.
- *Interrupt handlers*: `mx_irq_intc`, `mx_irq_dmac`, the vblank functions and
  `mx_timer_*` of `libmxk` go through the kernel (a table version), which keeps them per
  program and removes them at its end. A program that calls AddIntcHandler itself is not on
  `libmxk`'s rules (mx-os.md) and is not covered.
- *The units*: the DMAC's channels stopped and their interrupts cleared, VIF0/VIF1/GIF and
  VU0/VU1 reset, timers 0 and 1 stopped, the INTC mask as the kernel had it; the GS set up
  again by the kernel's screen.
- *Memory*: the VM's state, the heaps and the physical allocator reset by an explicit reset
  (now the restart clears them as `.bss`); the TLB written back as now.
- *Files, sockets, pad, sound*: the cleanup there is now, then without a restart.
- Test: the same program 50 times in a row (VM, threads, sound, network), and the BIOS's
  thread and semaphore counts the same after each.

  Done 2026-11-08 (kernel table version 8): on the PS2 `vm23_pad` 40 times, `vm24_home` 11,
  the VM tests (`vm11`, `vm12`, `vm13`, `vm16`) and `vm21_crash` (status 255) between them,
  Quake 600 frames twice, all without a restart; `tests/ps2/vm25_ids.c` (`probe/p34_ids.c` in
  VM) finds the same threads and as many semaphores after them as before. Interrupt handlers
  are removed from the program's own slots (`irq_tables`) rather than routed through the
  kernel: that comes with step 2's interrupts as events.

  The shell's game, then the shell, found three more things ExecPS2 did (Quake from USB, its
  swap on USB, quit from its menu; `tests/ps2/vm26_launch.c` starts programs as the shell does):
  the data cache held Quake's dirty lines, written back over the shell after its load (the
  shell ran its font); a `libmxk` from before version 8 does not show its interrupt slots, so
  its handlers stayed with the BIOS and ran in the shell's memory (such programs take the
  restart now); and calls to the IOP that the program's threads were waiting for, and their
  packets, are waited for before the next program is loaded.

**2b. An address space of its own** (one program at a time still). Every `libmxk` program of
table version 9 gets an ASID. Its core (text, data, bss, stack: what DMA may read, at
0x10_0000 up to its end) gets a physical run of its size from the kernel's frame pool and is
mapped there by entries of its ASID (G = 0), its uncached and UCAB views too; the run stays put
while the program lives, since its DMA chains hold physical addresses. Its VM is paged as now,
under its ASID. The kernel's memory below 1 MiB keeps the BIOS's global entries. Virtual to
physical is one place in maxsdk, `mx_paddr_of` (DMA tags, SIF transfers): it adds the core's
offset; so does the kernel for a program's pointers (file reads, SIF calls, the frame's chains).
A program whose run starts at its own addresses is as now.
- Test: the VM tests and Quake with their core run at another physical address, everything as
  before; a store through a wrong pointer past the program's end faults instead of reaching
  other memory.
- Programs on `libmx`, ps2sdk's and `libmxk` from before version 9 run alone on the BIOS's
  entries, as now.

  Started 2026-11-08: the ASID (2b-1). The kernel gives each `libmxk` program an ASID (1-255
  in turn) at its first call, and the VM's entries are written under it, not global
  (`tests/ps2/vm28_asid.c`: two runs in a row get 1 and 2, their VM entries carry them). On the
  PS2 the VM tests, the VM-inside tests (ASID 0) and Quake as before.

  2b-2, the core at a physical run of its own (kernel table version 9, 2026-11-08): a note in
  `libmxk` tells the kernel before the load, the kernel maps the core at the top of memory under
  the program's ASID and writes the offset into it; `mx_paddr_of`, `MX_UNCACHED` and `MX_UCAB`
  add it; the VM's frames are below the run; the main stack moved into the core (at the top of
  memory it was outside). On the PS2 the VM tests, `vm24_home auto` (the frame pipeline's DMA)
  and Quake (core at 0x1D0_0000) with moved cores, `tests/ps2/vm28_asid.c` (a DMA through the
  scratchpad finds the core's data). Found: a program cannot read the TLB itself, `tlbr` sets
  the CPU's ASID and its own code is then not mapped. Still open: a store past the core's end
  faults only above the run's 1 MiB rounding.

  2c-1 and 2c-2 (2026-11-08): a program without VM asks for a run with `MX_CORE_HEAP` (a second
  note); `mx_loader_launch` from such a program keeps it (`exec_park`): its kernel state saved
  in its core heap, its interrupt handlers taken from the BIOS and added again after (the slots'
  entry functions in `irq_tables`), its pad's ports closed, its other threads suspended; the
  other's run goes below it, and the call returns the other's exit status. mxshell asks for
  512 KiB and sets the GS, its font and covers and the pad up again after a game. On the PS2:
  `tests/ps2/k29_stay.c` with `vm26_launch` and Quake as the other (its memory, core offset and
  ASID as before), a missing program (-1), mxshell from `host:` starting Quake (back at once,
  its picture whole) and a program without a run (`vm5_tlb`: mxshell given up, as before), no
  thread or semaphore left (`vm25_ids`). mxshell's VM pages giving way to the game's (one frame
  pool for all) is not done: mxshell has no VM yet.

**2c. Processes side by side.** No program is limited to a share: mxshell (the shell,
`MXSHELL.ELF`) may grow as large as a game. Only the cores take fixed memory (mxshell's about
1.5 MiB, Quake's 1.7 MiB, each with its pinned memory); everything else is VM, paged from one
frame pool for all processes, with one swap file. mxshell stays when it starts a game: the
kernel suspends its threads (and saves VU0's registers), runs the game under the game's ASID,
and at its end sets mxshell's ASID again and resumes it, back at once and as it was. While
the game runs mxshell's VM pages give way to the game's (LRU over all processes).

**2d. The kernel's own scheduler.** The BIOS's threads know no processes: switching from one
process's thread to another's it keeps the ASID (the CPU's) and no VU0 register (`bios.md`), so
two processes cannot both have threads that run. The kernel schedules the threads of `libmxk`
programs itself: a thread list per process, the ASID and VU0's registers set at each switch, a
whole process suspended or resumed at once, its end without searching the BIOS's ids. For that
it takes over the interrupt dispatch and the return from an interrupt, where a switch happens
(the BIOS's level-1 vectors are in RAM; a `SetSyscall` handler runs in kernel mode). Its own
semaphores and sleep with it. Interrupts become the kernel's: a process's handlers run in its
address space, or as events (the frame pipeline waits for its vblank).

  The way there, each on the PS2 on its own:
  - *2d-1, the interrupt entry.* A probe puts the kernel's code at the interrupt vector
    (0x8000_0200, written from a `SetSyscall` handler, which runs in kernel mode), which saves
    what the BIOS's own entry saves, counts, and goes on into the BIOS's: nothing changes but
    that the kernel is first. Measured: the cost per interrupt.
    Probed 2026-11-08 (`probe/p37_irqhook.c`, `bios.md`): the vector takes its handler from a
    table in RAM (0x8001_5380: INTC index 2, DMAC 3), so the kernel only writes two words; a
    stub there saw every vblank, at no measurable cost. In the kernel the same day
    (`apps/mxos/ee/kirq.S`, `kirq.c`): its entry saves the registers C may change, runs
    `kirq()` on a stack of its own and goes on into the BIOS's handler; on while the kernel or
    a program built for it runs, the BIOS's table again before a program without the note or a
    loader. On the PS2 the VM tests, Quake, k29 and mxshell's paths as before, the entry
    counting every INTC and DMAC interrupt.
  - *How (chosen 2026-11-08: all threads the kernel's).* The BIOS's INTC handler
    (0x8000_0380, dumped) handles one cause per interrupt: it clears EXL, IE and KSU, saves the
    thread's context through BIOS routines, acknowledges the cause in I_STAT, clears a
    "reschedule" flag (0x8001_55B4) that the `i…` calls set, runs the cause's handlers and then
    switches BIOS threads if the flag is set. A kernel dispatch that only took the handlers over
    would still need that switch, at BIOS addresses of one version. So dispatch, threads and
    semaphores become the kernel's together: every thread of the kernel and of `libmxk`
    programs is a kernel thread, run on one BIOS thread (the carrier: the BIOS sees one thread
    and never switches); the kernel's interrupt entry dispatches the causes it owns itself (EPC
    and Status saved, EXL cleared as the BIOS does, so a TLB refill in a handler works), its
    handlers wake and signal with plain functions (no syscalls), and at the end of the entry, if
    a thread of higher priority is ready, it saves the interrupted thread's whole context, sets
    the next one's ASID and VU0 registers and returns into it by ERET. Causes it does not own go
    on into the BIOS's handler. A thread that waits switches in its own call (user mode). With
    nothing ready, an idle thread. maxsdk's `mx_bios_*` thread and semaphore calls are weak, the
    kernel's own replace them in the kernel; `libmxk` gets them as kernel services. Programs not
    on `libmxk` keep the BIOS's threads: they run alone, the carrier asleep meanwhile.
    Probed 2026-11-08 (`probe/p38_ksched.c`): the kernel's own switch at the return from an
    interrupt, two threads on one BIOS thread switched at each vblank by ERET into the other's
    saved context, works on the PS2.
  - *2d-2, the kernel's threads.* A thread record per thread (all registers, 128-bit GPRs, HI/LO,
    SA, the FPU; VU0's per process), ready queues by priority as the BIOS's, the switch at the
    return from an interrupt and in the kernel's calls (sleep, wake, wait, signal). The BIOS's
    threads stay for the kernel's own start and for programs not on `libmxk`; a `libmxk`
    program's threads are the kernel's (`mx_thread_*`, `<mx/sched.h>`, and the semaphores
    maxsdk uses: SIF waits, the VM lock, `mx_sleep_us`).
    Done 2026-11-08 (kernel version 10; `kernel.md`, Interrupts and threads): the threads,
    semaphores and interrupt handlers of the kernel and of `libmxk` programs of version 10 are
    the kernel's (`apps/mxos/ee/ksched.c`, `ksched.S`), on one BIOS thread; the kernel owns
    both interrupt lines and goes into the BIOS's handler no more; other programs run after a
    restart with the BIOS's threads. On the PS2 the VM tests (vm11-vm30), k29 with vm26 and
    with Quake as the other, and Quake itself. Learned: `mx_di` clears EIE, not IE (a context's
    interrupt state is both); the BIOS's `EnableIntc`/`EnableDmac` leave a cause off that has
    no BIOS handler; the handlers' order is the BIOS's (`next` 0 first: the kernel's screen
    acknowledges the GS's FINISH before a program's handler saw it, and the frame pipeline
    stopped); `SetupThread` gives the carrier's arguments.
  - *2d-3, processes.* A process is its threads, ASID and core run: the switch sets the ASID and
    VU0's registers when the next thread is another process's; a process is suspended and
    resumed whole (2c's `exec_park` on it), and its end is its list, not a search of the
    BIOS's ids.
    First part done 2026-11-08: every thread and semaphore has a process (the maker's; a
    program's first thread a new one, 0 the kernel's), and a program's end takes its process's
    instead of what was not there at its start. Before, the first program after a restart into
    the kernel's threads took a semaphore the kernel made while it ran, and left five.
    A parked process (2c) is suspended whole by the same lists. The switch itself (the ASID,
    the core's TLB slots, VU0's registers at a change of process) moves to 2f: until then one
    process runs and the other is parked with all its threads, so no switch goes from one to
    the other and the code could not be tried.
  - *2d-4, interrupts per process.* The INTC and DMAC handlers of a process run under its ASID
    when it is the current one, else its event is counted and seen when it runs again; the
    kernel's own (SIF, VM, home menu) always run. Moved to 2f with the switch, for the same
    reason: today the parked process's handlers are removed with it.

**2e. mxlink a program.** Worked out in [mx-os-apps-plan.md](mx-os-apps-plan.md) (the app model, the
kernel's interface for every app, the repos `apps/mxos`, `apps/mxlink`, `apps/mxshell`), whose steps
2e-1 - 2f-2 replace what follows here. `MXLINK.ELF` is the kernel and the host link in one. Split: the
kernel (`MXOS.ELF`: the boot, its services, processes, the scheduler, the home menu) and mxlink,
a `libmxk` program started at the boot beside mxshell: ps2link's protocol with ps2client,
`host:` files, the console and its screen. With 2d's threads it answers the host while a game
runs (now it cannot), and in step 3 it is a window like the others.

**2f. Both run.** mxshell draws while a game is paused (its threads suspended, its GS memory
kept), and the game goes on when mxshell gives it back: the base of step 3's windows.
Here two processes first run side by side, so 2d-3's switch (ASID, core TLB slots, VU0) and
2d-4's interrupts per process come with it (done in the apps plan's step 2e-2).

**2g. The BIOS only at the boot.** After 2d what the kernel still asks of the BIOS is its own
too: the SIF DMA queue (SifSetDma), the video mode (SetGsCrt), the caches, the program's start
(SetupThread, SetupHeap). Then the BIOS's memory (0x0 - 0x7_FFFF, its code and tables) is the
kernel's, all of the first MiB: room the kernel needs (mxlink's area 0x8_8000 - 0xF_F800 is
full). Programs on ps2sdk and `libmx`, which call the BIOS themselves, then run only after a
restart into the BIOS (ExecPS2 from the boot ROM again), or not.
  Done 2026-11-11 on the PS2 (kernel version 22): the caches, GS IMR and memory size without
  the BIOS (step 1); the video timing computed and written by the kernel (`<mx/crtc.h>`,
  `apps/mxos/ee/kvideo.c`); SIF1 through the kernel's own ring (`ee/src/base/sifdma.c`); a
  program on the kernel's threads starts without `SetupThread` and `SetupHeap` (crt0 sets its
  stack). Still the BIOS's: the threads' carrier, the exception and interrupt vectors' first
  level, `ExecPS2` for programs not on `libmxk`, and the kernel's own boot.

**2h. Only MXOS programs; the BIOS only boots** (decided 2026-11-11). Every program the kernel runs
is an MXOS program (`libmxk`, virtual memory); backwards compatibility with ps2sdk's programs and
with `libmx` programs under the kernel goes. One source builds two ways:

| | MXOS program (`libmxk`) | Standalone ELF (`libmx`, the portability library) |
|---|---|---|
| Started by | the kernel (the shell, the host link) | mxboot, a legacy loader, PCSX2's `-elf`, ps2link; not by the kernel |
| Memory | virtual only (`maxsdk-vm*.ld`), more than 31 MiB possible | physical, at most 31 MiB, no VM |
| BIOS | none while it runs | its threads, SIF, SetGsCrt (as `libmx` now) |
| IOP | the kernel's modules, shared | its own reset and modules |
| Kernel-only calls (processes, ports, launch, home menu) | yes | return -1 |

Steps, each on the PS2 on its own:
- *2h-1, two targets.* `MX_TARGET=mxos` (the default: `libmxk`, VM) or `standalone` (`libmx`,
  physical) in `mk/maxsdk.mk`. Tests that need no kernel (hardware, drivers, GS, VU) stay
  standalone and run on PCSX2 with `-elf`. Until every process has a VM of its own (the address
  space plan's phase 3) a program that stays while another runs cannot be VM: the kernel has
  one VM, the program in front's (a kernel test linked into VM that started another was dropped
  and started again, in a loop). Those (mxshell, mxlink's app, the kernel tests that start
  others or run beside them) are `mxos-core` until then; every other MXOS program is VM. In the
  end every MXOS program is VM (decided 2026-11-11).
  Done 2026-11-11: `MX_TARGET` (`mxos`, `mxos-core`, `standalone`); the kernel tests k37, k43,
  k44, k45 and `examples/display` are VM programs, k29-k36, k38, k40-k42, mxshell and mxlink's
  app `mxos-core`; the BIOS's programs (mxboot, the kernel) and the demos standalone. Found: a
  VM program that opened a path in its `.rodata` hung (the page fault under the file lock read
  the page through the file server, which waited for that lock): paths are copied into core
  before the lock now. Data the DMAC reads (the frame pipeline's MFIFO ring) must be marked
  core (`MX_DMA_LINES`) in a VM program. On the PS2 the kernel tests that start a program
  beside them fail while the host link's service runs: the kernel parks one program, and the
  service takes that place (on PCSX2, without the service, they pass).
- *2h-2, the kernel runs MXOS programs only.* An ELF without the MXK note is refused;
  `bios_mode` (the restart with the BIOS's threads) and the Exit hook go. Standalone ELFs are
  not started from MXOS for now (once the BIOS's memory is the kernel's there is no ExecPS2;
  later perhaps through a reset into mxboot's one-shot). A new kernel is tried through mxboot's
  one-shot, or a start of the kernel's own without ExecPS2.
  First part done 2026-11-11: `execee` of a new `MXKERNEL.ELF` replaces the running kernel
  (`kernel.md`; mxkernel-high is not needed for it any more). A software restart of the PS2
  through the ROM's reset vector was tried (`probe/p40_reboot.c`): the EE alone, with the IOP
  reset to the ROM's modules, gave a black screen (the ROM's EE path starts with the RDRAM's
  initialisation, which may fail on a warm machine; not followed further).
  Done 2026-11-11: the kernel refuses an ELF without the MXK note of version 10 and a loader;
  `bios_mode`, the loader's ExecPS2 and the Exit hook are gone (`kernel.md`). Tested on the PS2
  (booted by the one-shot, and by a replace): a standalone hello is refused, `k44` passes, the
  replace comes back with the shell on the screen. Card files are written with mxboot's
  `mxboot-install-mxos.elf` from now on.
- *2h-3, `libmxk` calls the kernel only.* The kernel counts the BIOS syscalls that still come
  in, by number; each `mx_bios_*` left in `libmxk` becomes a kernel service or goes.
  2h-3a done 2026-11-11 (the count, `kernel.md`), measured on PCSX2 over the MXOS tests (k29-k45,
  vm11-vm36, display): none makes a BIOS syscall but SetGsCrt (0x02: once in vm11, vm12, vm13,
  vm16, 18 times in display), which `gs.c` calls in the program when the kernel's video timing
  does not know the console (PCSX2's). Threads, semaphores, interrupt handlers and SIF DMA all
  go through the kernel already. Next (2h-3b): the BIOS's paths out of `libmxk` (they are
  `libmx`'s), SetGsCrt through the kernel only, and a build check that `libmxk.a` calls no
  `mx_bios_*`.
  2h-3b done 2026-11-11: so (`kernel.md`); `libmxk` keeps only the BIOS's `Exit`, for its stop
  without a kernel. Left for 2h-4: the BIOS under the kernel itself (its threads on the BIOS's
  carrier thread, its handlers in the BIOS's tables, its SIF software registers).
  2h-3c done 2026-11-12: an MXOS program does not build when it calls the BIOS (`kernel.md`):
  `<mx/bios.h>` only for standalone ELFs, no BIOS stubs in `libmxk`, the link checks the ELF
  for syscalls. What apps took from the BIOS: `<mx/thread.h>`'s sleep, wakeup and rotate (the
  kernel's threads), `mx_video_board_offset` through the kernel (version 23; the kernel asks
  the BIOS's GetGsDxDyOffset for every mode code once at its start and answers from that).
- *2h-4, the kernel without the BIOS.* Its own vectors (refill, common, interrupt) and syscall
  entry, its threads without the BIOS's carrier thread, the SIF's software registers its own.
  In parts, each tested alone (the kernel counts what still goes into the BIOS, `kernel.md`):
  - 4a, its own syscall entry: the exception table's syscall entry is the kernel's dispatcher,
    which runs its own numbers (the scheduler's switch, the poke, the VM's resume) and sends the
    rest on to the BIOS's; the TLB and exception handlers written into the table by the kernel
    (no SetSyscall, SetVTLBRefillHandler, SetVCommonHandler after its start). Done 2026-11-12:
    after `kirq_on` the kernel calls the BIOS only for SifGetReg (twice, at its start) and, on
    PCSX2, SetGsCrt; tested on PCSX2 (the MXOS tests, the standalone VM tests).
  - 4b, the SIF's software registers its own (SifGetReg/SifSetReg with bit 31). Done
    2026-11-12: a copy of the BIOS's table while the kernel has the SIF; after `kirq_on` the
    kernel's only BIOS call left is SetGsCrt on PCSX2.
  - 4c, the restart without ExecPS2: the kernel starts itself again (its `.data` back, `.bss`
    cleared, its threads and handlers anew), as the replace's trampoline does.
    Done 2026-11-12 for every restart but the first (which sheds the loader's state; 4e):
    tested on PCSX2 with the MXOS tests, `vm26_launch` and a chain of 20 `mx_loader_launch`es
    (the emu build follows a launch now). Left: `kirq_off` sets the poke by SetSyscall once a
    restart (gone with 4d-1).
  - 4d, its own level 1 vectors (refill, common, interrupt) and tables, out of the BIOS's
    memory; the BIOS's kernel stack and save area its own.
    4d-1 done 2026-11-12 on PCSX2 (`kernel.md`): the vectors and their tables are the kernel's,
    the BIOS's tables are no longer written, `kirq_off` puts the BIOS's vector code back. The
    MXOS tests pass on PCSX2 as before (k41_present and vm24_home stall in the frame pipeline
    on PCSX2 with any kernel, and standalone too: PCSX2's). Not yet on the PS2: the first try
    hung at the replace, with the instruction cache invalidated by kseg0 addresses (against the
    hardware restrictions; now by index).
    4d-2 done 2026-11-12 on PCSX2 (`kernel.md`): the vectors' save area and the syscall
    entry's stack are the kernel's; line 7 (the COP0 timer) still goes to the BIOS's handler,
    through a stub that gives it the BIOS's save area and counts it (0 on PCSX2: to check on
    the PS2, then a handler of the kernel's or the line off); exception codes 0, 11, 14 and 15
    to the kernel's crash report.
  - 4e, its threads without the BIOS's carrier thread, and its start without the BIOS's calls
    before `kirq_on` (the IOP's reset, the SIF's set-up).
    4e-1 done 2026-11-12 on PCSX2 (`kernel.md`): the kernel's own restart keeps its vectors and
    tables and calls no BIOS (SetupThread, SetupHeap, the handler and syscall sets, DisableDmac
    gone). 4e-2 done 2026-11-12: the first start's restart is the kernel's own jump too, no
    ExecPS2 (only the kernel's replace still uses it). Tested on PCSX2 with the PS2 build
    itself (not the emu build): a card image with `mc0:/MXOS`'s modules written by
    `mxboot-install-mxos` (format, copydir), PCSX2's sockets network for mxeth; it boots to
    ready and runs a program from the card (`vm38_vmbeside`'s waiting part; its part beside
    another fails there with and without 4e-2: the PS2 build relies on ASIDs, which PCSX2's
    TLB ignores; since the kernel measures that at its start, the PS2 build runs it whole on
    PCSX2). The first start reads its configuration after `kirq_on` and `ksched_init`, and its
    restart keeps the vectors: after the first `kirq_on` the kernel calls the BIOS only for
    the board's offsets (once) and, on PCSX2, SetGsCrt (measured with the PS2 build from a
    card image). The vectors go on in the start-up itself (start.c's weak `mx__kernel_early`,
    before `mx_exception_init`, in place of SetupHeap): the kernel's bootstrap is two BIOS
    calls, SetupThread (its arguments, the boot path) and the poke's SetSyscall (its way into
    kernel mode). Left: those two, and the BIOS thread the kernel's threads run on.
- *2h-5, the first MiB for the kernel.* The BIOS's memory (0x0 - 0x7_FFFF) is the kernel's
  after its boot. Not on PCSX2 (2026-11-12): there the kernel keeps calling SetGsCrt, whose
  code is in that memory (the BIOS's syscall table, SCPH-70004's image: syscall 2 at
  0x8000_BD58), and PCSX2 needs that call for the PAL rate (without it, PAL modes run at 60
  vblanks a second, `pcsx2.md`). On the PS2 the kernel's own CRTC timing makes it free.
- Then the address space plan's phase 3: VM in the kernel, one frame pool, one swap file
  (`mc0:/MXOS/SWAP.CFG`, no swap without it). Proposed steps (2026-11-12, not started):
  - 3a, a VM per process: vm.c's and image.c's state (about 28 KiB in the kernel now, the front
    program's only: resident.ld) becomes one instance per process, switched with the ASID
    where `kproc.c` switches the rest of a process's state; the refill already writes the
    process's ASID (G = 0). Test: a VM program beside another (`k31_beside` as `mxos`).
    Step 1 done 2026-11-12 (PCSX2): vm.c's tables of an address space are a `struct
    vm_space` (one, `space0`, through `vs`), apart from the VM's frames, swap file, threads,
    stats and trace; no change in behaviour. Next, as worked out: `vs` follows the running
    thread's process (switched where `kproc.c` switches the ASID), and at each switch the VM's
    TLB entries are cleared as at a vblank, so the TLB's table (`slot_page`) and `tlb.S`'s
    fast refill are always the current space's; the LRU (`oldest`, `evict`) and the writer
    take the victim's space explicitly (a frame's space next to `page_of`; stamps per space,
    `refills` the VM's). Where a space's tables live: the program's core heap (`table()`) is
    mapped per ASID, but tlb_init maps all of main memory globally at `MX_PHYS_WIN` (G = 1,
    cached), where the frames are reached; so a space's tables are pinned frames of the pool
    (`mx__vm_frames_take`), readable by every process's threads.
    Step 2 done 2026-11-12 (PCSX2): several spaces in vm.c (`mx__vm_space_new`, `_switch`,
    `_now`, `_free`): a frame's space next to its page (`frame_space`), stamps per space, the
    LRU, the writer and the moves (`vacate`, `gather`, `pin_run`) over the pages of every
    space; a switch clears the VM's TLB entries and points `tlb.S`'s fast refill at the new
    space. `tests/ps2/vm37_spaces.c`: two spaces over the same addresses on 20 frames, every
    word right after each switch (744 pages out, 680 in), a third space made and freed,
    `mx__vm_check` consistent.
    Step 3 done 2026-11-12 (PCSX2, `kernel.md`): a VM program may wait for or run beside
    another VM program. The kernel keeps its VM (`exec.c`'s `vm_kept`; `end.c` leaves
    resident.ld's `__vm_*` as they are), the next program's `mx_vm_init` makes it a space of its
    own (`kvm_init`), its core is a MiB-aligned run of the pool's frames, pinned, and `kproc.c`
    switches the space with the ASID. `tests/ps2/vm38_vmbeside.c`: 2 MiB of a pattern in VM,
    the same program started as a child that writes the same addresses, waited for and beside:
    the pattern intact after each. Found on the way: `mx_loader_launch` of libmxk shut its VM
    down before the kernel's launch (now the kernel does, only when it starts the other in its
    place); the kernel's `tlb_restore` at a program's end undid the VM's TLB (`mx__vm_tlb_again`
    after it); a core mapped while the VM is on lay in the VM's ring of TLB entries
    (`mx__vm_tlb_fix`); a child's `mx_exception_init` took the VM's handlers
    (`mx__vm_handlers_again`).
  - 3b: done for VM programs (one pool, one LRU over every space; a program's core pinned
    frames of it); the `mxos-core` programs keep their own run below the cores until 3d.
  - 3c done 2026-11-12 (PCSX2, `kernel.md`): `mc0:/MXOS/SWAP.CFG`, its size (64 MiB by
    default) a limit on the VM's slots, which a freed space gives back.
  - 3d begun 2026-11-12: the kernel tests that stay beside another (k29-k36, k38, k42) and
    mxlink's app (the service) are VM programs and pass on PCSX2 with and without the service.
    mxshell too (its DMA and IOP buffers in core): on PCSX2 it starts and reaches its game list
    as the core build did; to be seen on a TV before it goes on the card. `k40_frame` and
    `k41_present` too (k40 passes on PCSX2; the frame pipeline without VM is the standalone
    `frame` and `fpresent`'s). `MX_TARGET=mxos-core` is gone (`mk/maxsdk.mk` stops with a
    message): every MXOS program is VM.
  - 3b, one frame pool: frames from one pool for every process's VM, in place of each
    process's own run below the core of the one beside.
  - 3c, one swap file for all processes, from `mc0:/MXOS/SWAP.CFG`.
  - 3d, every MXOS program VM: mxshell, mxlink's app and the kernel tests that start others
    leave `MX_TARGET=mxos-core`, which goes.
  Decided (user, 2026-11-12): one LRU over the frames of all processes (the one in front gets
  the most by itself, the background shrinks); one swap file for MXOS, its size configurable,
  64 MiB by default.

- Then the address space plan's phase 4: no core. Decided (user, 2026-11-12): all programs are
  equal, as on Linux: the shell and mxlink's app too run at the same addresses on the same VM,
  a program that starts another waits for it (done for VM programs, `kernel.md`), and all
  memory is swappable but some of the kernel's.
  - Now: a program is two parts at the same virtual addresses in every process. Its core, from
    0x0010_0000 (`ee/ld/maxsdk-vm.ld`): crt0, every archive member (libmx, libmxc, picolibc,
    compiler-rt), the VU and HLSL objects, what is marked `MX_CORE_FN` / `MX_CORE_DATA`,
    packets, `MX_DMA_LINES` buffers, the main stack and the core heap; a MiB-aligned run of the
    pool's frames, pinned for the program's life (mxlink's app 1 MiB, mxshell 2-3 MiB, Quake 3
    MiB). Its VM, from 0x0800_0000: the program's own objects, 32 KiB pages, read from its ELF
    or zero filled, evicted by the one LRU to the one swap file. A parked program keeps its
    whole core in physical memory.
  - Why the core is there, and what takes its place:
    - DMA sees physical memory only (no IOMMU). In place: pinned pages, which the VM has
      already (`mx_vm_pin`, `mx_vm_lock`: resident, one physical run, `mx_paddr_of`). Packets,
      `MX_DMA_LINES` buffers and the IOP's RPC buffers become pinned at the load.
    - Code that runs with interrupts off (interrupt handlers, their callees, code under
      `mx_di`) may only touch resident pages: a fault there that needs the swap file ends the
      program. In place: such code and its data pinned (`MX_CORE_FN` / `MX_CORE_DATA` become
      "pinned"), pinned by the kernel at the load.
    - Thread stacks (the rule in `mx_thread_start`) and the main stack: pinned pages, pinned
      when the thread is made, unpinned at its end.
    - The libraries were core because the VM's own code ran in the program. The VM is the
      kernel's now (resident.ld's `__vm_*`), so libraries may be paged but for what the points
      above pin.
  - Kept fixed: the kernel below 1 MiB (code, data, vectors, its TLB entries), each address
    space's tables (pinned frames, read through `MX_PHYS_WIN`), and pages in a transfer.
  - Proposed steps (2026-11-12, not started):
    - 4a, measure: what each program has in core and why, by kind (libraries, stacks, DMA
      buffers, packets, handlers, the core heap), from its map file; mxlink's app, mxshell,
      Quake, the tests. The TLB's cost of paged code: refills now (Quake's timedemo on the PS2,
      code in core: 1.02 M refills, 178 k of code, about 610 ms).
    - 4b, pinned in place of core: `MX_PINNED_FN` / `MX_PINNED_DATA` (the `MX_CORE_*` names
      kept as aliases), sections the kernel pins at the load; a pinned heap from the pool's
      frames in place of the core heap (`<mx/mem.h>`); stacks pinned in VM. Archives move to
      VM in `maxsdk-vm.ld` but for the pinned sections. Test: every k/vm test, Quake's timedemo
      against 4a.
    - 4c, the core gone: the kernel's loader maps every segment as pages (from the ELF or zero
      filled) in place of `core_place`; crt0's first page is made resident before the jump; the
      virtual range from 0x0010_0000 is VM too. A program's physical memory is its pinned pages
      and the frames the LRU leaves it.
    - 4d, the background shrinks: a parked program keeps only what must stay (its handlers'
      code and data, buffers in a transfer); its packets and stacks of held threads may go.
  - Open: whether a program's code is paged while it is in front (more refills: 24 VM TLB
    entries of 32 KiB) or kept resident in front and paged only in the background; a check that
    finds code reached with interrupts off that is not pinned (a fault there is a crash, not a
    wait); VU microcode (`.vutext`) is sent by DMA: pinned or locked for the upload; programs
    without VM (only old binaries now) leave the kernel with this phase.

- The kernel no longer restarts (ExecPS2) after each program: it cleans up per process
  (threads, files, sockets, pad, sound, GS memory, interrupt handlers, VM frames).
- Each `libmxk` program gets an ASID; its core memory (now at the BIOS's fixed mappings from
  0x10_0000) is mapped per process, and `mx_paddr_of` adds the process's offset. The kernel's
  memory (below 1 MiB) stays global. The windows on all of main memory (`MX_PHYS_WIN`,
  uncached, UCAB) are only the kernel's.
- The kernel switches processes itself (the BIOS's scheduler does not know them): it suspends
  one's threads, sets the ASID, resumes the other's. Interrupts (vblank, GS, DMAC, timers)
  are the kernel's and reach programs as events.
- The shell (the GUI) and mxlink's host link screen run as processes beside a program. A
  program left in the background stays in memory, or goes to the swap file (16 MiB: about
  3.5 s to `host:`, 20 s to USB).
- Measured (`probe/p33_asid.c`, PS2, 2026-11-07; `bios.md`): ASIDs work (G = 0 entries match
  only their ASID, G = 1 all); the ASID survives syscalls and interrupts but is the CPU's, not
  a thread's: the kernel sets it at each process switch. The BIOS keeps no VU0 register (VF,
  VI) at a thread switch, the FPU's it does: the kernel saves VU0's at a process switch.
- Done: the VM's refill writes the process's ASID (`tlb.S` keeps EntryHi's, `vm.c`'s
  `tlb_write` ORs `mx__asid`; its entries G = 0); the BIOS's TLB entries for core memory are
  global and stay so for the kernel's memory below 1 MiB.

### Step 3: windows

- Graphics through the kernel: a program in a window sends its frame as a chain
  (`mx_frame_init_chain`), the kernel CALLs it after a prelude of the GS's drawing state
  (FRAME, ZBUF, SCISSOR, XYOFFSET, TEST of both contexts) and loads VU1's microcode again when
  another program's ran. The MFIFO ring and beam racing are for full screen only.
- `mx_gs_init*` in a window: the GUI's render target and a share of GS memory for the texture
  cache. The GUI draws each window's target as a texture.
- The pad goes to the window with the focus; sound only from it.
- Budget, GS memory (512 pages): the GUI's screen 640 x 448 CT16S twice, 140 pages; Quake in
  256 x 256 (CT32 twice, Z16), 80 pages; about 290 pages for textures and other windows.

### Step 4: full screen

- A window goes full screen and back: the program is told (an event), sets its framebuffer
  and video mode up again and loads its textures again; the GUI stops drawing meanwhile and
  keeps only the menu's GS memory. Quake: `VID_SetMode`.
