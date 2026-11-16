# mx OS: design, for porting games and apps

The kernel (`apps/mxos`, `MXKERNEL.ELF`; mxlink until 2026-11-10) is growing into a small resident OS for the PS2. A program uses what it
offers and keeps the graphics. Library split and service state: [kernel.md](kernel.md). Virtual
memory: [vm.md](vm.md). Where it goes (a game OS): [mx-os-plan.md](mx-os-plan.md).

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
  Between programs the kernel closes the ended program's files (not the swap file, nor the
  ELFs of the programs that go on: their pages come from them), pad ports and sound, and
  keeps the IOP and the network. USB pads (DualShock 3, DualShock 4, DualSense) run on usbd and
  maxsdk's `mxpad`, loaded at the boot (`docs/pad-bt-plan.md`), and with a Bluetooth dongle on USB
  the pads paired with it (over USB, as a PS3 or PS4 pairs them), kept in `mc0:/MXOS/BTPADS.DAT`. The internal HDD (`mxata`, `docs/iop-drivers.md`) is a FAT32 or exFAT
  disk like a USB stick: `ata0:`, with `PS2MAX/` as on the other devices. The shell's fourth
  device is `host:`: the directory of a `ps2client listen` on the PC.
- Boot: the memory card's system update is mxboot (`apps/mxboot`): its phase 1 starts
  `mc0:/MXBOOT/MXBOOT.ELF`, which starts the ELF that `MXBOOT/BOOTONCE.CFG` names (once: the file is
  removed first) or `MXBOOT/BOOT.CFG` (`mc?:/MXOS/MXKERNEL.ELF`). A new kernel or program is tried as
  the one-shot: if it hangs, a reset boots the default again. The kernel's directory is the one it
  was started from (`argv[0]` up to its last `/` or `:`: `mc0:/MXOS/`, `mc1:/MXOS/`, a one-shot's):
  its IOP modules, its settings and its shell are there. For a kernel started from `host:` (tried
  from the PC), or without `chime.irx` there, it is `MODULE_DIR`, `mc0:/MXOS/` (`make
  MODULE_DIR=...` for another). New IOP
  modules are tried the same way: a kernel with them in a directory of its own, as the one-shot.
- Memory cards: `mxmc.irx` (`iop/sio/mxmc`), MXOS's fork of ps2sdk's mcman with 16 open files for
  both slots in place of 3 (`docs/mxmc-plan.md`); the device and MCSERV's interface as mcman's. A
  card's size and free space are in the device list (`mx_dev_list`).
- Consoles: the kernel knows two (ROM 2.00, a SCPH-70004; ROM 2.20, a SCPH-90006: `kernel.md`,
  "Other consoles"). On those it sets the video modes itself, 576p (`mx_video_576p`) among them;
  on another a mode is the BIOS's or fails (`mx_video_set` returns -1: set another one). Run `examples/display` on each console the kernel is brought to.
- Settings (kernel version 27, `<mx/setting.h>`): one store of the system's settings,
  `mc0:/MXOS/SETTINGS.DAT` (keys such as `net.ip`, `swap.path`, `shell.path`, `service`,
  `storage.mx4sio`, `pad.swap_xo`, `video.mode`; values are text). Any program reads them
  (`mx_setting_get`, `mx_setting_list`); only the shell changes them (`mx_setting_set`), and every
  process with events gets `MX_EVENT_SETTING`. The kernel writes the file a few seconds after the
  last change. The files below (`SERVICES.CFG`, `SHELL.CFG`, `SWAP.CFG`, `IPCONFIG.DAT`) are only
  imported once, when there is no store yet.
- Services: the kernel starts the program `mc0:/MXOS/SERVICES.CFG` names (a path and its
  arguments) at the boot and after a restart; it stays beside every program in front
  (`mx_proc_service`, kernel version 18). The host link is one (kernel version 20):
  `mc0:/MXOS/MXLINK.ELF` takes ps2client's commands but the reset (version 21; 20: execee
  and the ping), ends the shell for them and starts the host's program; without it the kernel
  serves only the reset, execee and the ping.
- Devices (kernel version 26, `<mx/dev.h>`): the kernel loads the storage drivers at its boot
  (bdm, its FAT/exFAT file system, USB drives, the HDD) and keeps them; a program never loads a
  storage IRX. MX4SIO is off: it shares slot 2 with a memory card and keeps the card from being
  seen (the setting `storage.mx4sio` = `on` loads it at the next boot). Devices come and go at any time: `mx_dev_list` (name,
  kind, present, file system, size, free, flags `MX_DEV_SWAP`, `MX_DEV_BUSY`), `mx_dev_gen`,
  `mx_dev_eject` (refused while the swap file or open files are on it), and the hint
  `MX_EVENT_DEVICE` to every process with `mx_proc_events`. Free space of FAT and exFAT is not
  known yet (0).
- Controllers (kernel version 28, `<mx/pad.h>`): `mx_pad_info` (kind, link, battery, charging,
  light colour, id) for each of `MX_PAD_PORTS`, `mx_pad_gen` and `MX_EVENT_PAD` on a change,
  the Bluetooth pairings (`mx_pad_paired`, `mx_pad_pair_state`); the shell pairs, forgets and
  turns pads off (`mx_pad_pair`, `mx_pad_forget`, `mx_pad_disconnect`). Settings: `pad.idle_off`
  (minutes), `pad.brightness` (0..100), `pad.<id>.colour` (`rrggbb`).
- Time (kernel version 29, `<mx/time.h>`): `mx_time` (UTC seconds; 0 when the PS2's clock could
  not be read), `mx_gmtime`, `mx_localtime` (settings `time.zone` in minutes, `time.dst`); the
  shell sets it with `mx_time_set`, which writes the PS2's clock too.
- Network (kernel version 30, `<mx/net.h>`): `mx_net_status` (link, speed, address, DHCP,
  lease, MAC) for any program, `MX_EVENT_NET` on a change. The shell sets it up with the
  settings `net.mode` (`dhcp` or `manual`), `net.ip`, `net.mask`, `net.gw`, `net.dns`; the
  kernel applies them a second after the last change.
- Video (kernel version 31, `<mx/screen.h>`): the shell's settings say what the TV shows:
  `video.mode` (240p, 288p, 480i, 576i, 480p, 576p, 720p, 720p50, 1080i, 1080i50, 1080p24,
  1080p25, 1080p30; default the region's 480i or 576i; the 50 Hz and 1080p modes are the
  kernel's own timing, not the BIOS's), `video.out` (`component`, or `rgb`: 240p, 288p, 480i
  and 576i only), `video.scale` (`stretch`: every
  program in `video.mode`; `native`: a program may take any mode of `video.allowed`),
  `video.aspect` (4:3 or 16:9, for 240-576 line modes), `video.overscan` (`on`: 640 wide, 224,
  256, 448 or 512 lines; `off`: all of 704 x 240, 288, 480 or 576). The kernel's own screen is
  in `video.mode` (170 columns on 720 and 1080 line modes). A change of `video.mode` or `video.out` is on trial: unless the
  shell writes `video.confirm` = "<mode> <out>" within 15 s, the kernel puts back the confirmed
  ones. L1 + L2 + R1 + R2 held in the first 30 s after the start sets the region's 480i or
  576i. A program shows its picture at whole factors (MAGH, MAGV) in a mode it may take:
  `mx_screen_pick` for the size it draws, `mx_screen_video` for the mode to set; `mx_gs_init`
  does it for its w x h (480i the same as before). The GS scales by whole numbers only, so
  other sizes get black borders. A program that sets a mode of its own (`mx_gs_init_video`)
  still can: that is not checked.
- Notifications (`<mx/notify.h>`): the kernel keeps the last 32 and adds its own (a device came
  or went, a controller came or went or its battery is low, the swap file); any program adds one
  with `mx_notify`. The shell shows them (`mx_notify_read`, `MX_EVENT_NOTIFY`); over a running
  game only after plan step 5. `MX_EVENT_PAD` tells of a controller's change. The controller ports
  (SIO2) are watched while a program has them open (`mx_pad_init`), USB and Bluetooth pads always.
- The swap file (`mx_swap_set`, `mx_swap_info`; the shell only): attached when its device is
  there, at the boot or when it comes, never waited for. Notifications: created, in use (urgent:
  do not remove the device), its device removed (urgent: a program whose pages were only in it
  ends with "its page was in the swap file of a device that was removed"), none set (urgent:
  MXOS soon runs out of memory).
- Memory (kernel version 26, plan step 4e): the kernel turns the VM on at its boot. All memory
  from 1 MiB is its frames; each program's core is a run of them (pinned) and its VM an address
  space of its own. A program that would take all memory (without VM and without a core heap
  of its own) is not started.
- The shell: the program the setting `shell.path` names (kernel version 27; before, `SHELL.CFG`), else
  `mc0:/MXOS/MXSHELL.ELF`. It is a role, not a file name: the kernel starts it when nothing else
  is in front and brings it in front at the home button, and `mx_proc_list` marks it
  (`MX_PROC_SHELL`; a service `MX_PROC_SERVICE`).
- Start: the kernel loads the ELF (from `host:`, `usb0:`, `mx4sio0:`, `ata0:`, ...) and runs it in a thread
  at priority 64: one of the kernel's own threads for a `libmxk` program of version 10, which
  uses the kernel's threads, semaphores and interrupt dispatch (`kernel.md`, Interrupts and
  threads); any other ELF (a standalone one on `libmx`, ps2sdk's, an older `libmxk`, a loader
  such as ps2link) is refused: mxboot or a legacy loader starts those. End: back to the kernel, then the shell (`SHELL.CFG`'s, by default `mc0:/MXOS/MXSHELL.ELF`) if it started
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
6. **Swap.** There is one swap file, MXOS's: the settings `swap.path` and `swap.size` (MiB, 64 by
   default; before kernel version 27 `mc0:/MXOS/SWAP.CFG`) name it, shared by all address spaces, and the VM uses at most that size:
   a program that needs more ends with "the swap file is full" (plan phase 3c). Since kernel
   version 26 a program's own `mx_vm_config.swap` (and a launch's) is not used; without
   it VM programs have only the frames. Never truncate or assume its size. A program the host
   link starts may bring its own file on `host:` for that run (`host:mxswap.txt`, read by
   mxlink's app at execee; kernel version 28): MXOS's is used again after it.
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
    and interrupt handlers go on while the other runs: they may not use the pad meanwhile,
    and the other runs in front. Since 2026-11-14 (mx OS plan step 4c) a program beside may
    end (`mx_exit`, returning from `main`) or crash while the other runs: it ends alone and
    the one in front goes on; before, its end ended the one in front. `mx_proc_front(id)`
    (version 24) brings a process that waits or runs beside in front; the one in front is
    paused (threads stopped, its memory, kernel state and VU0 registers kept) until that one
    ends or it is brought back. While a program waits (for the one it started, or paused),
    its interrupt handlers do not run and its timers fire only when it is in front again.
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
    on USB) and FAT keeps a file's size until it is closed: close files. On a memory card keep
    few files open at a time: the card driver (mxmc) has 16 for both slots, and each running
    program's ELF from a card takes one (the service's and the shell's too); past that -24 (EMFILE).
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
program on maxsdk's frame pipeline, else it ends it (`mx_pad`). Since kernel version 25, when the
shell started the program (and waits for it or runs beside it, `mx_proc_spawn`), the home button
brings the shell in front instead: the program is paused at its frame boundary and the shell gets
`MX_EVENT_HOME`; it brings the program back with `mx_proc_front` or ends it with `mx_proc_kill`.
With the shell in front the home button is the shell's: it does not end the shell. Each gets its
own video mode back at these switches; what it drew in GS memory is not kept. It is the system's: any pad's
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
