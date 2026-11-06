# mx-kernel and its two libraries

The services a program uses have one interface, the `<mx/...>` headers, and two
implementations. A program chooses one when it links (`MX_MEMORY`, `mk/maxsdk.mk`):

| | `libmx.a` | `libmxk.a` |
|---|---|---|
| `MX_MEMORY` | `phys` | `vm` |
| Runs | on a bare PS2: ps2link, PCSX2, and under mxlink | under mx-kernel |
| TLB | the BIOS's | the kernel's |
| Memory | 31 MiB, physical | a 128 MiB virtual window, and core memory |
| Files, pad, sound | its own SIF client | the kernel's |

A program's source is the same for both; only the room differs. What only makes
sense with virtual memory exists in `libmx` too, doing nothing where nothing is
needed, so programs and libgl need no `#ifdef`. Graphics (GS, VIF, DMA) are not a
service: they are direct code in both.

The kernel is `tools/mxlink`, resident at 0x88000-0xFF800. `libmxk` calls it through
`struct mxk_services` (`<mx/kernel.h>`), a table the loader passes. Only `libmxk`
sees how a call reaches the kernel, so that can change without changing programs.

## Service groups

| Group | Headers | State |
|---|---|---|
| Program | `<mx/loader.h>`, `mx_exit` | the kernel's (version 1) |
| SIF | `<mx/sif.h>` | the kernel's (version 2) |
| Files | `<mx/sif.h>` (`mx_fio_*`) | program's own client, on the kernel's SIF; moves with virtual memory, which it knows |
| Memory | `<mx/vm.h>`, `<mx/mem.h>` | program's own |
| IOP | `<mx/iop.h>`, `<mx/sif.h>` | over the kernel's SIF |
| Pad, sound | `<mx/pad.h>`, `<mx/snd.h>` | program's own, over the kernel's SIF |
| Network | to be defined | |
| Threads, exceptions | `<mx/thread.h>`, `<mx/sched.h>` | program's own |

`libmxk` without a kernel (PCSX2, ps2link) works as `libmx` does for now: the
services not yet in the kernel are in it. That ends when virtual memory moves.
