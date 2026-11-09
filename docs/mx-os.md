# mx OS: design, for porting games and apps

mxlink (`apps/mxos`) is growing into a small resident OS for the PS2. A program uses what it
offers and keeps the graphics. Library split and service state: [kernel.md](kernel.md). Virtual
memory: [vm.md](vm.md). Where it goes (programs in windows): [mx-os-plan.md](mx-os-plan.md).

## Model

- One foreground program at a time. A `libmxk` program runs under an ASID of its own, its VM
  entries only its own; one linked into VM (`MX_MEMORY=vm`) and built for kernel version 9 also
  has its core at a physical run of its own (the top of memory: `mx-os-plan.md`, step 2b). The
  DMAC uses physical addresses, so a program can still write anywhere: protection is against
  accidents only, and the kernel's code is read-only for user mode.
- The IOP boots once. Its modules live in `mc0:/MXOS/`; storage drivers are loaded for the
  device in use by the shell. The network runs on maxsdk's drivers (mxdev9, mxeth, mxnet:
  `docs/iop-drivers.md`), and only on them: none on a PCMCIA PS2 (SCPH-1xxxx). The fast path for
  `host:` files is its own module beside mxnet (mxfast, which is the console too: to the
  ps2client that serves the fast path, without loss, else by broadcast as udptty sent it).
  Between programs mxlink closes files, pad ports and sound, and
  keeps the IOP and the network. USB pads (DualShock 3, DualShock 4, DualSense) run on usbd and
  maxsdk's `mxpad`, loaded at the boot (`docs/pad-bt-plan.md`), and with a Bluetooth dongle on USB
  the pads paired with it (over USB, as a PS3 or PS4 pairs them), kept in `mc0:/MXOS/BTPADS.DAT`. The internal HDD (`mxata`, `docs/iop-drivers.md`) is a FAT32 or exFAT
  disk like a USB stick: `ata0:`, with `PS2MAX/` as on the other devices. The shell's fourth
  device is `host:`: the directory of a `ps2client listen` on the PC.
- Boot: the memory card's system update is mxboot (`apps/mxboot`): its phase 1 starts
  `mc0:/MXOS/MXBOOT.ELF`, which starts the ELF that `MXOS/BOOTONCE.CFG` names (once: the file is
  removed first) or `MXOS/BOOT.CFG` (`mc?:/MXOS/MXLINK.ELF`). A new mxlink or program is tried as
  the one-shot: if it hangs, a reset boots the default again. New IOP modules the same way: an mxlink built
  with `MODULE_DIR=mc0:/MXOS/NEW/` reads them (and its shell) from that directory.
- Start: mxlink loads the ELF (from `host:`, `usb0:`, `mx4sio0:`, `ata0:`, ...) and runs it in a thread
  at priority 64: one of the kernel's own threads for a `libmxk` program of version 10, which
  uses the kernel's threads, semaphores and interrupt dispatch (`kernel.md`, Interrupts and
  threads); for every other program mxlink first starts again with the BIOS's threads. End: back to mxlink, then the shell (`mc0:/MXOS/MXSHELL.ELF`) if it started
  the program. A `libmxk` program (version 8 on) ends without a restart: the kernel removes its
  threads, semaphores and interrupt handlers, waits for its IOP calls, resets the units and
  empties the caches (`kernel.md`, The end of a program).

| Who | Owns |
|---|---|
| Kernel (mxlink) | IOP, SIF, program start and end, TLB and VM, heaps, files, exceptions, the pad, threads, semaphores and interrupt dispatch (`libmxk`); later sound |
| Program | GS, VIF, VU, DMAC, its frame loop |

## Memory (EE)

| Range | Use |
|---|---|
| `0x00000-0x87FFF` | BIOS, do not touch |
| `0x88000-0xBFFFF` | kernel data and stack |
| `0xC0000-0xFF7FF` | kernel code, read-only for programs |
| `0xFF800-0xFFFFF` | host commands |
| `0x100000-0x1FFFFFF` | program (core memory) and VM frames; a moved core (above) is at the top, the frames below it |
| `0x08000000-` | VM window, `MX_MEMORY=vm` |

Retail PS2s have 32 MiB; never count on more.

## Rules for a port

1. **Link.** `MX_MEMORY=vm` (libmxk, 128 MiB virtual) for anything large, `MX_KERNEL=1` with `phys` (libmxk, all in core), `phys` alone (libmx, 31 MiB, no VM, also without a kernel)
   otherwise. Same source for both, no `#ifdef`.
2. **Paths are relative.** A path without a device is in the directory of the ELF, so one ELF runs
   from `host:` and `usb0:`. Never write `host:` or a device name in a game; make engine base
   directories `.`.
3. **No IOP work.** No IOP reset, no module loading, no `rom0:` drivers: `mx_pad_init` and
   `mx_snd_init` bind to what runs (the kernel boots mxsnd: `kernel.md`, Sound). Loading a
   module forces a full IOP reset at return.
4. **No loader calls.** No `ExecPS2`, `LoadExecPS2` or BIOS `Exit`. End with `mx_exit`, `exit()` or
   a return from `main`; to start another program, `mx_loader_launch`. `mx_exit` ends the threads
   of `mx_thread_start`, without a chance to clean up.
5. **Leave the TLB and the exception handlers alone.** Interrupt handlers only through `mx_irq_*`
   (`<mx/irq.h>`): the kernel removes those when the program ends, not ones added to the BIOS directly.
6. **Swap.** Set `mx_vm_config.swap` to a relative name; the shell's shared
   `<device>/PS2MAX/swap.bin` replaces it. Never truncate or assume its size.
7. **DMA.** Tags, chains, DMA buffers, thread stacks and interrupt handler data must be core or
   pinned (`mx_vm_lock`, `mx_vm_pin`, `MX_CORE_FN`, `MX_CORE_DATA`), never plain VM. A physical
   address only from `mx_paddr_of` / `mx_paddr_range`, an uncached view only from `MX_UNCACHED` /
   `MX_UCAB`: a program's core need not be at its own addresses (`mx__core_off`), so never mask a
   pointer (`& 0x0FFFFFFF`) or OR in `0x20000000` yourself. Nor read the TLB (`tlbr` changes the
   CPU's ASID while your own code is under it).
11. **Starting another program.** `mx_loader_launch` ends a program, or, with its core at a run
    of its own and without VM (`MX_CORE_HEAP`, as mxshell), returns the other's exit status when
    that ends: then set up again what the other had (the GS and the display, the pad's ports:
    `mx_pad_init`, files). The main stack is in the core after `.bss` (128 KiB), not at the top
    of memory. `mx_loader_launch_beside` (kernel version 12) is the same, but the program's
    other threads and interrupt handlers go on while the other runs: they may not end the
    program (`mx_exit`) or use the pad meanwhile, and the other runs in front.
    `mx_proc_spawn` (version 14) starts it without a waiting thread: `mx_proc_wait` for its
    end and exit status, `mx_proc_kill` to end it. `mx_proc_events` (version 15) sends the
    kernel's events, such as that end, as messages to a port of yours. `mx_log_read` (version
    16) reads every process's `mx_printf` output. A system app with an IOP module of its own
    (rule 3 is for games) gets what the module sends with `sceSifSendCmd` as messages:
    `mx_iop_channel` (version 17).
12. **Talking to another program** (kernel version 13): through the kernel, never through
    memory, which is your own (another program's addresses are its own). `<mx/proc.h>` lists
    the processes; `<mx/msg.h>`'s ports: make one by name (`mx_port_create`), receive and
    reply on it in a thread of your own; any process finds it (`mx_port_find`) and sends,
    waiting for the reply. At most `MX_MSG_MAX` (256) bytes each way, copied by the kernel;
    `MX_MSG_SLOTS` (4) messages under way at once, more senders wait. Your ports go with your
    end, and so do requests you did not answer (their senders get -2).
8. **Graphics are yours**: full control of GS, VIF, VU and DMA, but the display registers (PMODE,
   DISPFB, DISPLAY, BGCOLOR) only through maxsdk (`<mx/display.h>`, `mx_gs_*`): under `libmxk` the
   kernel writes them (`kernel.md`). Prefer beam racing for memory
   (`<mx/hires.h>`).
9. **A pad is all there is.** Every prompt must work without a keyboard: Cross confirms where an
   engine asks for `Y`. Quit must be reachable from the menu.
10. **Saves and configs** go next to the ELF, by relative path. Writes are slow (about 750 KB/s
    on USB) and FAT keeps a file's size until it is closed: close files.
  Files go through mxio when the card has it (`kernel.md`): nothing changes for a program, but keep
  read buffers 16-byte aligned and sizes whole quadwords: anything else takes a copy on the EE.

Sockets: `<mx/net.h>`, for the LAN. No call waits (`MX_NET_AGAIN`), so a game polls each frame; at most
4 KiB moves per call. To wait, `mx_net_wait`: the thread sleeps until a socket can go on. A port with BSD
socket code adds `$(MX_NET_CFLAGS)`: `socket`, `select`, `fcntl` and the rest are in libmxc
(`ee/libc/net.c`), blocking unless made non-blocking; a blocking call sleeps, it does not spin. Not there: DNS (dotted
addresses only), `MSG_PEEK`, `FIONREAD`, broadcast, and options, which are accepted and ignored.

Waiting: sleep, do not spin. `mx_sleep_us` (`<mx/thread.h>`) lets the other threads run; `mx_delay_us`
keeps the CPU. Timers are a service (`<mx/timer.h>`; under the kernel its, version 19): never
write an EE timer by its number. `mx_timer_add` calls a function once or every period (in core,
from an interrupt); `mx_timer_claim` lends a whole EE timer for a clock of its own (as
`mx_hires` does) and says which, or -1. The profiler is a software timer.

Provided for every program: the home button (`MX_PAD_HOME`: a USB pad's PS button; on a SIO2 pad
Select + Start + L1 + R1 held together, which the program then gets as the home button, not as
those four) opens the kernel's home menu (resume, the pad's settings, quit) for a `libmxk`
program on maxsdk's frame pipeline, else it ends it (`mx_pad`). It is the system's: any pad's
home button reaches whichever port a program reads. L3 + R3 toggles libgl's counters, off by
default. Do not bind these.

The pads a program reads (`mx_pad_*`): first the SIO2 pads that are there, in port order, then
the USB pads in the order they came; a pad plugged into a SIO2 port moves the USB pads up a port
(a SIO2 pad holds its port for half a second after it was last seen, so plugging it in or out
moves the others once).
A USB pad's Share (Create) is Select, Options is Start; its sticks and buttons are the SIO2 pad's
(`<mx/pad.h>`). `mx_pad_rumble` runs a pad's motors (USB, Bluetooth or a SIO2 DualShock, which it puts in
DualShock mode) until the next call; the kernel stops them when the program ends.

## Packaging

```
<device>/PS2MAX/<game>/
  <game>.elf
  game.ini     title=, year=, type=, elf=, args=, description=, cover=
  cover.t8     apps/mxshell/tools/mk-cover.py
  <game data>
<device>/PS2MAX/swap.bin
```

All paths in `game.ini` are relative to its directory; a directory with just `<dir>.elf` is a game
too. `apps/mxshell/package-games.sh` writes this layout.

## Testing a port

- Without the PC: start it from the shell. A test with `ps2client` connected can hide `host:` use:
  look for `open name host:` in the run's log, it must be absent.
- A run ends when the program comes back to mxlink, however it ended: mxlink prints the end marker
  (`@@PS2TEST_END@@ status=N`) with its exit status. A game prints no marker of its own: the host
  would go away while the game still ends. `MX_TEST_END` is silent under mxlink.
- Quit must return to the shell with status 0. A crash ends with status 255, and the shell
  shows the kernel's report (`kernel.md`, Crashes).
- Run on the real PS2 at 32 MiB (`/ps2-hw-test`). PCSX2 needs `PCSX2_EE=interp` for VM.
