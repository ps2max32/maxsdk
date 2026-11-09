# mx OS: apps, services and the kernel's interface (plan)

Where [mx-os-plan.md](mx-os-plan.md) step 2e ("mxlink a program") leads to once mxlink is more than a
split: an app model that the shell, mxlink, the games and later windowed programs all follow,
and one interface between the kernel and every app. Agreed 2026-11-08.

## Decided

- mxlink becomes an ordinary `libmxk` program with an address space (ASID) of its own
  (option B): its core at a run of its own, without VM (as mxshell; one VM, the front's). So
  the process switch of 2d-3 (ASID, core TLB slots, VU0) and 2d-4's interrupts per process
  come in this step, not in 2f: two processes really run side by side here.
- mxlink is an app the shell shows like the others. It runs as a service: started at the
  boot, or not, as chosen in the shell.
- The interface between the kernel and mxlink is the kernel's general app interface. Nothing
  in it is for mxlink alone: what mxlink does, another app could do the same way.
- The kernel, mxlink and the shell are apps under `ps2max/apps/`, each in its own repo.
  `tools/` is for the PC side.
- The shell is **mxshell** (below), the only name for it from now on: in the code, the docs,
  the card's files and the repo.

## The shell's name

It was called mxloader in some places and mxshell in others (`MXSHELL.ELF`, `shell/`). "Loader" no
longer fits: the kernel loads programs, and the shell becomes the launcher and window manager.
Chosen: mxshell. The other names weighed:

| Name | For | Against |
|---|---|---|
| **mxshell** | already the ELF (`MXSHELL.ELF`) and the source directory; "shell" covers launcher and window manager | "shell" also suggests a command line |
| mxhome | it is what the home button leads to, and the home screen | "home" is also the button and the menu over a game |
| mxdesk | the window manager, desktop | a PS2 with a pad has no desk; the launcher matters more |
| mxlaunch | says what it does today | too narrow once it manages windows |

## The repos

| Repo (new place) | Holds | From |
|---|---|---|
| `apps/mxos` (new, ps2max32/mxos) | the kernel `MXOS.ELF`: boot, processes, scheduler, VM and TLB, interrupts, pad, the kernel's text screen (boot, crash reports), the services list; the IOP modules of the kernel (`mxroot`, `chime`); filling `mc0:/MXOS` (`mxos-fill`) | `tools/mxlink/ee` (`cmd.c` until 2e-4), `iop/mxroot.c`, `iop/chime.c`, `tools/mxos-fill.c` |
| `apps/mxlink` (moved, ps2max32/mxlink, history kept) | the host link app `MXLINK.ELF`: ps2link's protocol with ps2client, its window (the log), and ps2link's IOP module | `tools/mxlink/iop`; `ee/cmd.c` from apps/mxos in 2e-4 |
| `apps/mxshell` (new, ps2max32/mxshell) | the shell: launcher, home menu, later the window manager; `mk-cover.py`, `package-games.sh` | `tools/mxlink/shell`, its tools |
| `libs/maxsdk` (stays) | `libmxk`, `<mx/kernel.h>` and the new interface's headers; the docs: `mx-os.md` (the rules for programs), `kernel.md` (the kernel's services as `libmxk` calls them: it goes with `<mx/kernel.h>`), the plans | |

- The new repos are split off with their history (`git filter-repo` on a copy, kept to their
  own paths), and in one commit the moved files leave mxlink.
- mxboot runs on the PS2 too: it moves to `apps/mxboot` as it is (a repo without a remote).
- These change with the move: `build-tools.sh`, `build-apps.sh`, `build-all.sh`, the CLAUDE.md
  files, the test skill (paths to the loader, installing), the CI workflows and the READMEs.

## The app model

Three kinds, one way to start them (the kernel's `spawn`):

| Kind | Screen | Example |
|---|---|---|
| full screen | all of GS memory and the video mode, as programs have now. One at a time, in front; the home button pauses it (its frozen frame as a texture, its GS memory saved: the mockup) | the games, ps2sdk and `libmx` programs |
| windowed | a surface the kernel gives it; the shell shows it | step 3's programs in windows |
| service | runs without being in front, a text console as its window | mxlink, later a file or network server |

**Text consoles** (decided 2026-11-09, in place of EE surfaces for services): a service is a
command line program. What it says is its `mx_printf`, in the kernel's log with its process
(version 16); the shell shows a console window of it, the log's pieces of that process in its
own font, with a few KiB of lines kept. What it is told comes as messages to a port of its
own (`<name>.in`): lines the shell sends, typed or picked with the pad. It costs the service
no memory for a screen (an EE surface of 512 x 256 CT16 would be 256 KiB in each service,
and uploads to the GS), and needs no GS arbitration.

**GS surfaces**, for programs that draw: a render target in GS memory, drawn by the GS through
the kernel's queue (step 3: the chain CALLed after a prelude of FRAME, SCISSOR, XYOFFSET), one
or two buffers. This is for games in a window, later. EE surfaces (a front buffer in the
app's memory that the shell uploads) are dropped for now; they come back only if an app that
draws cannot use the GS.

**A manifest per app**, `app.ini` (it replaces `game.ini`, which keeps working): the name, the
kind, the surface size (windowed), the cover, and whether a service starts at the boot by
default.

- System apps are in `mc0:/MXOS/APPS/<id>/`: mxlink, the shell.
- The other apps are in `<device>:/PS2MAX/<id>/`, as now.

**Services at the boot**: `mc0:/MXOS/SERVICES.CFG` lists the ids that start. The kernel reads it.
The shell edits it (Settings > Services: on at the boot or not, start now, stop now). The shell
itself is not in it: the kernel always starts it last, in front.

## The interface: the kernel and every app

One set of calls in `struct mxk_services` (version 12 and on), and `libmxk`'s wrappers in new
headers (`<mx/proc.h>`, `<mx/msg.h>`, `<mx/log.h>`; `<mx/surface.h>` with step 3). Each group
has a test of its own. Where
mxlink needs something, it is general:

| Group | Calls | mxlink uses it for |
|---|---|---|
| Processes | `spawn(path, argv, flags: front, service)`, `exit`, `wait` (the exit status), `kill`, `suspend`, `resume`, `list` (id, name, kind, state, memory) | `execee` (spawn in front), `reset` (kill the program in front), `status` |
| Messages | named ports: `port_create("mxlink")`, `port_find`; `send` (request, reply: synchronous), `receive`, `reply`; copied by the kernel from one address space to the other, up to 4 KiB a message | the shell asks mxlink for its state; the kernel's events |
| Events | the kernel's messages to a process's own port: the focus, close, suspend and resume, the home button (to the shell), a process's end (to whoever waits) | its end, close |
| The log | every process's `mx_printf` to the kernel's log, with its process; any process may read it from where it last was | the console to the host, and its window |
| IOP channels | an app registers a SIF channel, and what its IOP module sends there reaches its port (instead of a fixed EE address, as ps2link's 0xFF800 is now) | ps2link.irx's commands |
| Memory of another process | `proc_read`, `proc_regs`, for debuggers | `dumpmem`, `dumpreg` |
| Consoles | the log (its output) and a port `<name>.in` (its input); events: shown, hidden, focus | its window |
| Surfaces (step 3) | `surface_create(w, h, psm)` in GS memory, `surface_title`; events: shown, hidden, focus | |
| Input | the pad of the process in front or with the focus; the home button to the shell | |

- **Who may do what**: for now every app may use every call; the manifest's `system=1`
  (mxlink, the shell) is the first place a limit can go.
- **The host's `host:` files** stay as they are: the IOP serves them, apart from this interface.

## Steps (in place of 2e and 2f in mx-os-plan.md)

1. **2e-1, the repos.** Move and split as above, with no change in behaviour (the loader is still
   one ELF, `MXLINK.ELF`, now built in `apps/mxos`). Then the build scripts, the docs, the skill
   and CI. On the PS2: the usual series.
   Done 2026-11-08: `apps/mxos` and `apps/mxshell` split off with their history, mxlink and mxboot
   moved; the kernel's build output is the same bit for bit, and on the PS2 the RAM chain with
   hello, vm28 and vm24 passes. `ee/cmd.c` stays in mxos until 2e-4, and mxos builds
   `ps2link.irx` from `apps/mxlink`.
2. **2e-2, the process switch.** The ASID, the core's TLB slots, VU0's registers and the
   interrupts per process at a switch to another process's thread. Test: two VM programs side
   by side (a counting service beside vm28 and Quake), each checking its own memory.
   Done 2026-11-09 (kernel version 12, `apps/mxos/ee/kproc.c`, `kernel.md`): a program without
   VM starts another with `mx_loader_launch_beside` and goes on beside it; the one beside has
   no VM (the VM's state stays the front's). `k31_beside` beside `vm32_side`, `vm28_asid` and
   Quake (600 frames, 1 % slower; the worker 33 000 rounds meanwhile), each checking its
   memory at the same virtual addresses, VU0's registers, its ASID and its vblank handler.
   Learned: threads run in user mode, so the side's copy of the state is read under its own
   ASID, not through kseg0; the kernel's handlers act on the front (the VM's vblank in the
   side's space wrote into the other's memory); DMA lines (the pad's) are never exchanged (the
   IOP died after 60 Quake frames); `tlb_restore` beside the side keeps the BIOS's entries
   outside its range (the kernel reads modules to 0x0100_0000).
3. **2e-3, the interface, part 1.** Processes, messages, events, the log, IOP channels, each
   with a test of two processes.
   - Done 2026-11-09 (kernel version 13): `proc_self`, `proc_list`, and ports and messages
     (`<mx/proc.h>`, `<mx/msg.h>`; `apps/mxos/ee/kmsg.c`). Test: `k32_port` serves a port
     beside `vm33_msg`, which checks the list, sends requests of every length from VM and
     from core, and ends without answering one of `k32_port`'s (it gets -2). On PCSX2: the
     kernel's PCSX2 build now runs programs side by side (its TLB ignores the ASID: the
     kernel writes the entries again at a switch).
   - Learned: the kernel has 2 KiB left below its code (`0xC0000`, the rest is read-only to
     threads, which run in user mode), so a message is at most 256 bytes and 4 are under way
     until 2e-4 frees the command thread's 32 KiB. A program turning an interrupt off turned
     it off for the other process too (now: only when no other process has a handler there).
   - Done 2026-11-09 (kernel version 14): `mx_proc_spawn`, `mx_proc_wait`, `mx_proc_kill`; test
     `k33_spawn` with `vm34_child` (it ends by itself with status 7 while the other polls;
     it runs until ended with status 42; a missing program gives -1). Learned: timer 0 was
     each program's (`mx_sleep_us`): the end of one stopped the other's sleepers, so it is
     the kernel's now; the end's `units_off` stops it, so the kernel sets it again; a core
     program's `argv` was in the kernel's memory, the next program's arguments once it
     started another; `core_vm` of the other stayed after its end.
   - Done 2026-11-09 (kernel version 15): events, the kernel's messages to a port of the
     process (`mx_proc_events`), the first a program's end with its exit status; test
     `k35_events` with `vm34_child`. Focus, close, suspend and the home button come with the
     surfaces (2f).
   - Done 2026-11-09 (kernel version 16): the log, every process's `mx_printf` and the
     kernel's output in a ring of 8 KiB that any process reads (`mx_log_read`); test `k36_log`.
     The room came from the kernel's debug hold buffer (16 KiB, unused by it).
   - Done 2026-11-09 (kernel version 17): IOP channels, an IOP module's SIF commands (cid
     0-31) as messages to a port (`mx_iop_channel`); test `k37_iopchan` with its own module
     (`tests/iop/chantest`). Step 2e-3 is done; all of it is tested on PCSX2 only.
   - Left for later: `proc_read`/`proc_regs` (with 2e-4's `dumpmem`/`dumpreg`), surfaces and
     input (2f), more and larger messages (2e-4 frees the room), a service started beside
     the shell (the kernel starts one at the boot: 2e-4).
4. **2e-4, mxlink an app.**
   - Done 2026-11-09 (kernel version 18, PCSX2 only): services. `mx_proc_service` makes a
     program the one beside for good; the kernel starts `SERVICES.CFG`'s at the boot, then
     the shell in front; a program's end goes to it as an event and the shell comes next
     unless it starts one. Test `k38_service`.
   - Done 2026-11-09 (PCSX2 only): mxlink's app skeleton (`apps/mxlink/ee`), a text service:
     events, its port, its IOP channel, the log.
   - Done 2026-11-09 (kernel version 19, PCSX2 only): timers as the kernel's service, software
     timers for every process and EE timers lent out (`mx_timer_add`, `mx_timer_claim`); the
     shell's hires claims one. Test `k42_timers` with `vm35_timers`. The shell and its console
     run on PCSX2 now.
   - Done 2026-11-09 (kernel version 20, on the PS2): the host link's first commands in the app.
     The kernel passes ps2client's execee and dumpreg (the ping) to the process with the IOP
     channel `MX_IOP_CHANNEL_HOST`; the app ends the shell for them and starts the host's
     program, and prints its end marker. ps2link.irx keeps its DMA to `0xFF800`: a SIF command
     packet (112 bytes) is too small for execee's 256 bytes of arguments, and the DMA reaches the
     kernel whatever program runs. The kernel keeps the rest, and serves everything while no
     service has the channel (a program that takes all memory ends the service; a program on the
     BIOS's threads restarts the kernel). On the card: `MXLINK-APP.ELF` and `SERVICES.CFG`.
     Learned: the kernel's file calls had no lock without VM (a service's tty write and the
     kernel's sent a packet twice); a thread ended while woken for a semaphore took the lock with
     it; the TLB entries kept for the one beside stayed after a program that takes all memory
     ended it; reading the modules from the card again after the shell ended failed (its drivers):
     a shell ended for the host keeps the IOP, as the takeover did.
   - Done 2026-11-09 (kernel version 21, on the PS2): `mx_phys_read` and `mx_phys_write`, the
     EE's physical memory copied by the kernel in kernel mode; the app serves dumpmem, writemem,
     gsexec and the VU commands. The kernel keeps the reset and the IOP's exception, and its
     `cmd.c` as the fallback while no service runs. Test `k43_phys`; on the PS2 a writemem and
     dumpmem of 64 KiB match. Learned: a shell ended without the restart left something running
     that crashed the next program at once (not found yet): the restart, as the takeover did.
   - Done 2026-11-09: the kernel's `cmd.c` keeps only the fallback for when no service runs
     (a program on the BIOS's threads, one that takes all memory, mxlink-high): the reset,
     execee and the ping; dumpmem, writemem, gsexec and the VU commands are the app's only
     (16 KiB of the kernel's memory free). The rest of `cmd.c` stays while programs can push
     the service out.
   - Next: the names: `MXOS.ELF` the kernel, `MXLINK.ELF` the app (a service in
     `SERVICES.CFG`), with mxboot's `BOOT.CFG`, the skill and the card's install.
5. **2f-1, the home menu over a paused game** (the mockup): the shell draws, with the frozen frame
   as one texture, while the game's GS memory is saved.
6. **2f-2, consoles.** The shell's console window of a service (the log of its process, its
   input port), mxlink's among them, and Settings > Services.
7. Then step 3 (GS surfaces, games in windows) and 2g (the BIOS only at the boot), as in
   mx-os-plan.md.

## Open points

- **Memory**: mxlink takes its memory (its core: code and buffers, no screen; the skeleton is
  22 KiB) from what games have now. The kernel's area below 1 MiB gets back what moves out.
- **Message size**: copies of up to 4 KiB keep the kernel simple (256 bytes until 2e-4 frees
  the room). Larger data (a file block) would need a shared page granted to the other
  process.
- **mxlink while no network is up**: it stays a service without a link, and its window says so.
