# mx OS: design, for porting games and apps

mxlink (`tools/mxlink`) is growing into a small resident OS for the PS2. A program uses what it
offers and keeps the graphics. Library split and service state: [kernel.md](kernel.md). Virtual
memory: [vm.md](vm.md).

## Model

- One foreground program at a time. No process isolation: same address space, and the DMAC
  uses physical addresses, so a program can always write anywhere. Protection is against
  accidents only: the kernel's code is read-only for user mode.
- The IOP boots once. Its modules live in `mc0:/MXOS/`; storage drivers are loaded for the
  device in use by the shell. Between programs mxlink closes files, pad ports and sound, and
  keeps the IOP and the network.
- Start: mxlink loads the ELF (from `host:`, `usb0:`, `mx4sio0:`, ...) and runs it in a thread
  at priority 64. End: back to mxlink, then the shell (`mc0:/MXOS/MXSHELL.ELF`) if it started
  the program.

| Who | Owns |
|---|---|
| Kernel (mxlink) | IOP, SIF, program start and end, TLB and VM, heaps, files, exceptions; later pad, sound |
| Program | GS, VIF, VU, DMAC, its frame loop |

## Memory (EE)

| Range | Use |
|---|---|
| `0x00000-0x87FFF` | BIOS, do not touch |
| `0x88000-0xBFFFF` | kernel data and stack |
| `0xC0000-0xFF7FF` | kernel code, read-only for programs |
| `0xFF800-0xFFFFF` | host commands |
| `0x100000-0x1FFFFFF` | program (core memory) and VM frames |
| `0x08000000-` | VM window, `MX_MEMORY=vm` |

Retail PS2s have 32 MiB; never count on more.

## Rules for a port

1. **Link.** `MX_MEMORY=vm` (libmxk, 128 MiB virtual) for anything large, `MX_KERNEL=1` with `phys` (libmxk, all in core), `phys` alone (libmx, 31 MiB, no VM, also without a kernel)
   otherwise. Same source for both, no `#ifdef`.
2. **Paths are relative.** A path without a device is in the directory of the ELF, so one ELF runs
   from `host:` and `usb0:`. Never write `host:` or a device name in a game; make engine base
   directories `.`.
3. **No IOP work.** No IOP reset, no module loading, no `rom0:` drivers: `mx_pad_init` and
   `mx_snd_init` bind to what runs. Loading a module forces a full IOP reset at return.
4. **No loader calls.** No `ExecPS2`, `LoadExecPS2` or BIOS `Exit`. End with `mx_exit`, `exit()` or
   a return from `main`; to start another program, `mx_loader_launch`.
5. **Leave the TLB and the exception handlers alone.**
6. **Swap.** Set `mx_vm_config.swap` to a relative name; the shell's shared
   `<device>/PS2MAX/swap.bin` replaces it. Never truncate or assume its size.
7. **DMA.** Tags, chains, DMA buffers, thread stacks and interrupt handler data must be core or
   pinned (`mx_vm_lock`, `mx_vm_pin`, `MX_CORE_FN`, `MX_CORE_DATA`), never plain VM.
8. **Graphics are yours**: full control of GS, VIF, VU and DMA. Prefer beam racing for memory
   (`<mx/hires.h>`).
9. **A pad is all there is.** Every prompt must work without a keyboard: Cross confirms where an
   engine asks for `Y`. Quit must be reachable from the menu.
10. **Saves and configs** go next to the ELF, by relative path. Writes are slow (about 600 KB/s
    on USB) and FAT keeps a file's size until it is closed: close files.
  Files go through mxio when the card has it (`kernel.md`): nothing changes for a program, but keep
  read buffers 16-byte aligned and sizes whole quadwords: anything else takes a copy on the EE.

Sockets: `<mx/net.h>`, for the LAN. No call waits (`MX_NET_AGAIN`), so a game polls each frame; at most
4 KiB moves per call. A port with BSD socket code adds `$(MX_NET_CFLAGS)`: `socket`, `select`, `fcntl` and
the rest are in libmxc (`ee/libc/net.c`), blocking unless made non-blocking. Not there: DNS (dotted
addresses only), `MSG_PEEK`, `FIONREAD`, broadcast, and options, which are accepted and ignored.

Provided for every program: Select + Start + L1 + R1 ends it (`mx_pad`); L3 + R3 toggles libgl's
counters, off by default. Do not bind these combinations.

## Packaging

```
<device>/PS2MAX/<game>/
  <game>.elf
  game.ini     title=, year=, type=, elf=, args=, description=, cover=
  cover.t8     tools/mxlink/tools/mk-cover.py
  <game data>
<device>/PS2MAX/swap.bin
```

All paths in `game.ini` are relative to its directory; a directory with just `<dir>.elf` is a game
too. `tools/mxlink/package-games.sh` writes this layout.

## Testing a port

- Without the PC: start it from the shell. A test with `ps2client` connected can hide `host:` use:
  look for `open name host:` in the run's log, it must be absent.
- A run ends when the program comes back to mxlink, however it ended: mxlink prints the end marker
  (`@@PS2TEST_END@@ status=N`) with its exit status. A game needs no marker of its own.
- Quit must return to the shell with status 0. A crash ends with status 255, and the shell
  shows the kernel's report (`kernel.md`, Crashes).
- Run on the real PS2 at 32 MiB (`/ps2-hw-test`). PCSX2 needs `PCSX2_EE=interp` for VM.
