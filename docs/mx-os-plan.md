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
  with status 0; 2026-11-07, seen by the user). Not yet tried: Quake.

### Step 2: processes

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
- To measure first: does the BIOS keep VU0's registers (COP2) at a thread switch; ASIDs with
  the BIOS's vectors and the VM's fast refill (`tlbr` replaces EntryHi's ASID). PS2 only: PCSX2
  gets `tlbwr` and `tlbp` wrong.

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
