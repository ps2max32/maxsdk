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

  1b-1d written 2026-11-07 (kernel table version 7; `tools/mxlink/ee/home.c`,
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
  other's run goes below it, and the call returns the other's exit status. mxloader asks for
  512 KiB and sets the GS, its font and covers and the pad up again after a game. On the PS2:
  `tests/ps2/k29_stay.c` with `vm26_launch` and Quake as the other (its memory, core offset and
  ASID as before), a missing program (-1), mxloader from `host:` starting Quake (back at once,
  its picture whole) and a program without a run (`vm5_tlb`: mxloader given up, as before), no
  thread or semaphore left (`vm25_ids`). mxloader's VM pages giving way to the game's (one frame
  pool for all) is not done: mxloader has no VM yet.

**2c. Processes side by side.** No program is limited to a share: mxloader (the shell,
`MXSHELL.ELF`) may grow as large as a game. Only the cores take fixed memory (mxloader's about
1.5 MiB, Quake's 1.7 MiB, each with its pinned memory); everything else is VM, paged from one
frame pool for all processes, with one swap file. mxloader stays when it starts a game: the
kernel suspends its threads (and saves VU0's registers), runs the game under the game's ASID,
and at its end sets mxloader's ASID again and resumes it, back at once and as it was. While
the game runs mxloader's VM pages give way to the game's (LRU over all processes).

**2d. The kernel's own scheduler.** The BIOS's threads know no processes: switching from one
process's thread to another's it keeps the ASID (the CPU's) and no VU0 register (`bios.md`), so
two processes cannot both have threads that run. The kernel schedules the threads of `libmxk`
programs itself: a thread list per process, the ASID and VU0's registers set at each switch, a
whole process suspended or resumed at once, its end without searching the BIOS's ids. For that
it takes over the interrupt dispatch and the return from an interrupt, where a switch happens
(the BIOS's level-1 vectors are in RAM; a `SetSyscall` handler runs in kernel mode). Its own
semaphores and sleep with it. Interrupts become the kernel's: a process's handlers run in its
address space, or as events (the frame pipeline waits for its vblank).

**2e. Both run.** mxloader draws while a game is paused (its threads suspended, its GS memory
kept), and the game goes on when mxloader gives it back: the base of step 3's windows.

**2f. The BIOS only at the boot.** After 2d what the kernel still asks of the BIOS is its own
too: the SIF DMA queue (SifSetDma), the video mode (SetGsCrt), the caches, the program's start
(SetupThread, SetupHeap). Then the BIOS's memory (0x0 - 0x7_FFFF, its code and tables) is the
kernel's, all of the first MiB: room the kernel needs (mxlink's area 0x8_8000 - 0xF_F800 is
full). Programs on ps2sdk and `libmx`, which call the BIOS themselves, then run only after a
restart into the BIOS (ExecPS2 from the boot ROM again), or not.

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
- Still to do: the VM's refill writes EntryHi with ASID 0 (`tlbwi` after setting EntryHi), so
  its entries become per process only when it writes the process's ASID; the BIOS's TLB
  entries for core memory are global and stay so for the kernel's memory below 1 MiB.

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
