# mx OS: design, for porting games and apps

The kernel (`apps/mxos`, `MXKERNEL.ELF`; mxlink until 2026-11-10) is growing into a small resident OS for the PS2. A program uses what it
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
  Between programs the kernel closes files, pad ports and sound, and
  keeps the IOP and the network. USB pads (DualShock 3, DualShock 4, DualSense) run on usbd and
  maxsdk's `mxpad`, loaded at the boot (`docs/pad-bt-plan.md`), and with a Bluetooth dongle on USB
  the pads paired with it (over USB, as a PS3 or PS4 pairs them), kept in `mc0:/MXOS/BTPADS.DAT`. The internal HDD (`mxata`, `docs/iop-drivers.md`) is a FAT32 or exFAT
  disk like a USB stick: `ata0:`, with `PS2MAX/` as on the other devices. The shell's fourth
  device is `host:`: the directory of a `ps2client listen` on the PC.
- Boot: the memory card's system update is mxboot (`apps/mxboot`): its phase 1 starts
  `mc0:/MXBOOT/MXBOOT.ELF`, which starts the ELF that `MXBOOT/BOOTONCE.CFG` names (once: the file is
  removed first) or `MXBOOT/BOOT.CFG` (`mc?:/MXOS/MXKERNEL.ELF`). A new kernel or program is tried as
  the one-shot: if it hangs, a reset boots the default again. New IOP modules the same way: a kernel built
  with `MODULE_DIR=mc0:/MXOS/NEW/` reads them (and its shell) from that directory.
- Services: the kernel starts the program `mc0:/MXOS/SERVICES.CFG` names (a path and its
  arguments) at the boot and after a restart; it stays beside every program in front
  (`mx_proc_service`, kernel version 18). The host link is one (kernel version 20):
  `mc0:/MXOS/MXLINK.ELF` takes ps2client's commands but the reset (version 21; 20: execee
  and the ping), ends the shell for them and starts the host's program; without it the kernel
  serves only the reset, execee and the ping. A program that
  takes all memory (no core heap of its own) ends the service; it starts again after that one.
- Start: the kernel loads the ELF (from `host:`, `usb0:`, `mx4sio0:`, `ata0:`, ...) and runs it in a thread
  at priority 64: one of the kernel's own threads for a `libmxk` program of version 10, which
  uses the kernel's threads, semaphores and interrupt dispatch (`kernel.md`, Interrupts and
  threads); any other ELF (a standalone one on `libmx`, ps2sdk's, an older `libmxk`, a loader
  such as ps2link) is refused: mxboot or a legacy loader starts those. End: back to the kernel, then the shell (`mc0:/MXOS/MXSHELL.ELF`) if it started
  the program. A `libmxk` program (version 8 on) ends without a restart: the kernel removes its
  threads, semaphores and interrupt handlers, waits for its IOP calls, resets the units and
  empties the caches (`kernel.md`, The end of a program).

| Who | Owns |
|---|---|
| Kernel (mxkernel) | IOP, SIF, program start and end, TLB and VM, heaps, files, exceptions, the pad, threads, semaphores and interrupt dispatch (`libmxk`), the video timing (`mx_video_set` writes it through the kernel from version 22: any timing `<mx/crtc.h>` describes, not only the BIOS's modes); later sound |
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

1. **Link.** `MX_TARGET=mxos` (the default; `MX_MEMORY=vm` says the same): an MXOS program on
   libmxk, linked into VM (128 MiB virtual; `MX_VM_TEXT=core` keeps its code in core), the only
   kind the kernel runs. `MX_TARGET=standalone` (`MX_MEMORY=phys`): a standalone ELF on libmx, the
   portability library: the same API on the BIOS, all in physical memory (at most 31 MiB, no VM),
   started by mxboot, a legacy loader or PCSX2's `-elf`, not by the kernel; the kernel's own calls
   (processes, ports, launch, the home menu) return -1. Same source for both, no `#ifdef`.
   libmxk calls no BIOS (a build check, `kernel.md`); libmx is the one with the BIOS's paths.
   An MXOS program makes no BIOS calls either, and does not build with one (plan step 2h-3c):
   `<mx/bios.h>` is for `MX_TARGET=standalone` only (an `#error` otherwise, and `<mx/mx.h>` no
   longer includes it), libmxk has no BIOS stubs (a link error), and the link checks the ELF
   for syscall instructions (`tools/mxos-bios-check.sh`, from `$(call link)`). What a program
   needs of the BIOS is the kernel's: threads in `<mx/thread.h>` (`mx_thread_sleep`, `_wakeup`,
   `_rotate`), the board's video offsets in `mx_video_board_offset` (kernel version 23).
   Every MXOS program is VM (`MX_TARGET=mxos`): one that stays while another runs (the shell,
   a service, one that starts others and waits) has an address space of its own on the
   kernel's frames and swap file (plan phase 3a, `kernel.md`); what the DMAC, the VUs or the
   IOP read is in core (`MX_CORE_DATA`, `MX_CORE_BSS`, `MX_CORE_FN`). Since plan phase 4b the
   libraries' code and data are VM too, but for what runs with interrupts off or before the VM
   is on; each link checks that (`tools/pinned-check.py`: what a handler or code after a `di`
   calls or reads must be core). `mxos-core` (all in core) went with phase 3d; all in physical
   memory is a standalone ELF.
2. **Paths are relative.** A path without a device is in the directory of the ELF, so one ELF runs
   from `host:` and `usb0:`. Never write `host:` or a device name in a game; make engine base
   directories `.`.
3. **No IOP work.** No IOP reset, no module loading, no `rom0:` drivers: `mx_pad_init` and
   `mx_snd_init` bind to what runs (the kernel boots mxsnd: `kernel.md`, Sound). Loading a
   module forces a full IOP reset at return.
4. **No loader calls.** No `ExecPS2`, `LoadExecPS2` or BIOS `Exit`. End with `mx_exit`, `exit()` or
   a return from `main`; to start another program, `mx_loader_launch`. `mx_exit` ends the threads
   of `mx_thread_start`, without a chance to clean up. No `SetupThread` or `SetupHeap` either:
   maxsdk's crt0 sets the main stack, and on the kernel's threads (`libmxk`, kernel version 10 and
   up) the start-up asks the BIOS nothing (`kernel.md`).
5. **Leave the TLB and the exception handlers alone.** Interrupt handlers only through `mx_irq_*`
   (`<mx/irq.h>`): the kernel removes those when the program ends, not ones added to the BIOS directly.
6. **Swap.** Set `mx_vm_config.swap` to a relative name; the shell's shared
   `<device>/PS2MAX/swap.bin` replaces it. Never truncate or assume its size.
   With `mc0:/MXOS/SWAP.CFG` (a path, then the size in MiB, 64 by default) the kernel gives
   every program that one file in place of its own, shared by all address spaces, and the VM
   uses at most that size: a program that needs more ends with "the swap file is full"
   (plan phase 3c).
7. **DMA.** Tags, chains, DMA buffers, thread stacks and interrupt handler data must be core or
   pinned (`mx_vm_lock`, `mx_vm_pin`, `MX_CORE_FN`, `MX_CORE_DATA`, `MX_CORE_BSS`), never plain
   VM: `mx_paddr_of`, `MX_UNCACHED` and `MX_UCAB` of VM that is not pinned end the program with a
   report (status 255; before phase 4b `MX_UNCACHED` of VM reached other memory). A physical
   address only from `mx_paddr_of` / `mx_paddr_range`, an uncached view only from `MX_UNCACHED` /
   `MX_UCAB`: a program's core need not be at its own addresses (`mx__core_off`), so never mask a
   pointer (`& 0x0FFFFFFF`) or OR in `0x20000000` yourself. Nor read the TLB (`tlbr` changes the
   CPU's ASID while your own code is under it).
11. **Starting another program.** `mx_loader_launch` waits: an MXOS program (VM) stays, also
    with a service beside (the host link on the PS2; since 2026-11-12, kernel 23), and the call
    returns the other's exit status when that ends (only an old program without VM may still be
    ended and replaced): then set up again what the other had (the GS and the display, the pad's ports:
    `mx_pad_init`, files). The main stack is in the core after `.bss` (128 KiB, or
    `--defsym=__mx_stack_size=<bytes>`; not `__stack_size`), not at the top of memory.
    `mx_loader_launch_beside` (kernel version 12) is the same, but the program's other threads
    and interrupt handlers go on while the other runs: they may not end the program
    (`mx_exit`) or use the pad meanwhile, and the other runs in front.
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
moves the others once). A SIO2 pad counts only with the configuration mode (DualShock, DualShock 2):
digital PS1 pads and guns are not read, as a slim PS2's port 0 answers like one with nothing
plugged in (probably its IR receiver) and took port 0 from the USB pads.
A USB or Bluetooth pad's light shows the port it is (DS3 LED, DS4 light bar colour, DualSense
player lights): the kernel tells mxpad.irx (`MXPAD_PLAYERS`) when the order changes.
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
- A run ends when the program comes back to the kernel, however it ended: the kernel (or the host link's app beside it) prints the end marker
  (`@@PS2TEST_END@@ status=N`) with its exit status. A game prints no marker of its own: the host
  would go away while the game still ends. `MX_TEST_END` is silent under the kernel. A program the kernel
  refuses (not an MXOS program) ends the run with status 127.
- Quit must return to the shell with status 0. A crash ends with status 255, and the shell
  shows the kernel's report (`kernel.md`, Crashes).
- Run on the real PS2 at 32 MiB (`/ps2-hw-test`). PCSX2 needs `PCSX2_EE=interp` for VM.
