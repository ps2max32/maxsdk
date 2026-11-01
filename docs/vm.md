# Virtual memory (`<mx/vm.h>`)

Optional: a program that calls `mx_vm_init` (or defines `mx_vm_config`) gets up to 128 MiB of
virtual memory at 0x0800_0000, in 32 KiB pages held in the physical memory left after the core
(the frames) and, when they run out, in a swap file. Without it nothing changes.

## Design

| Part | What |
|---|---|
| Core | The program (text, data, bss), the main stack, the core heap (`mx_core_alloc`, `mx_dma_alloc`, from the top of the heap), the VM's tables. On the BIOS's fixed mappings: it never faults. DMA tags, chains, rings, thread stacks, interrupt handler data must be core. |
| VM | 0x0800_0000 – 0x0FFF_FFFF: not RAM, so a VM pointer taken for a physical address faults instead of reaching other memory. `malloc` (libmxc's `sbrk` → `mx_sbrk`) hands out VM when VM is on. |
| `malloc` | libmxc's own (`libc/malloc.c`, TLSF: free blocks in lists by size class, found by bitmaps), not picolibc's: that one clears every block it hands out and walks one address-sorted free list on every `free`. Over VM both touch pages all over the heap, each touch maybe a swap transfer: Quake's 16 MiB hunk was written out to swap as it was allocated, and its loading thrashed (1700 page faults a second, no progress). Now `malloc` touches the block's header, its neighbours' and a list head; it does not clear (`calloc` does; new VM pages are zero) |
| Page | 32 KiB = one TLB entry, both 16 KiB halves valid (PageMask 16 KiB). An entry with an invalid half hangs the PS2 (below), so a page is in the TLB whole or not at all, and every miss is a refill. |
| TLB | `tlbwi` only, at entries 31–47 in turn, with a table of what each holds (no `tlbwr`, no `tlbp`: PCSX2 gets both wrong). The BIOS's entries 0–30 stay; its UCAB entries (31–38) may be replaced and are put back on a miss. 17 entries: 544 KiB in the TLB at once. |
| Exception | Codes 1–3 (`SetVTLBRefillHandler`) go to `src/vm/tlb.S`: the registers are saved, C (`mx__vm_exception`, at EXL = 1, on its own stack, core memory only) does as little as it can: it refills the TLB, maps a resident page again, or marks the page dirty (TLB modified). Everything else goes to the thread. |
| Page fault | A new page (zero fill) or one in the swap file continues in the faulting thread: the registers are copied onto its stack, the exception returns into `mx__vm_slow` with the thread's interrupts, which takes the VM lock (a BIOS semaphore: other threads run meanwhile), zero-fills, or evicts and reads, then resumes by a negative syscall whose handler (at EXL = 1) restores everything and returns to the instruction. A fault with interrupts off, in the BIOS, or on a VM stack ends the program with a report: touch VM that may not be resident only from thread code with interrupts on (pin what an interrupt handler reads). |
| Eviction | The clock (second chance) over the frames: a used page is taken out of the TLB and kept; an unused one goes, written to its swap slot if dirty, dropped if clean. Pinned and busy pages are skipped. |
| Dirty tracking | Clean pages are mapped without D: the first store raises TLB modified (code 1) and marks the page. `mx_vm_init` tests it: on PCSX2, which ignores D, every mapped page counts as dirty. |
| Swap file | Through the IOP file server (`host:`, `mass:`, …), opened read-write and truncated; slots are given out in order, so the file only grows at its end; one 32 KiB read or write a page. |
| DMA | The DMAC sees physical addresses only (`<mx/dma.h>`, below). |

### Programs linked into VM

* `MX_MEMORY=vm` (`mk/maxsdk.mk`) links with `ld/maxsdk-vm.ld`: the program's own objects
  (text and rodata, then data and bss, each from a page) at 0x0800_0000; crt0, every archive
  member (libmx, libmxc, picolibc, compiler-rt), libmxc's glue, the VU and HLSL objects, VU
  microcode, constructors, packets, `MX_DMA_LINES` buffers, `mx_vm_config`, and what the
  program marks `MX_CORE_FN` / `MX_CORE_DATA` stay core. `jal` reaches between the two.
* Started by `mxload.elf host:app.elf args...` (`loader/mxload.c`, linked at 0x01F0_0000):
  it loads the core segments itself, leaves the VM ones, and jumps to the program in its own
  thread, argv[0] the program's path. The start-up turns VM on (`mx_vm_config`, or no swap
  file and every frame), opens argv[0] and registers the VM segments' pages with the pager
  (`src/vm/image.c`), and starts the VM heap after them. Started otherwise, the program stops
  with a message.
* Those pages are read from the ELF when first touched (`file_in`, the stats' `file_ins`),
  the bytes past a segment's file size zero. One not written since is dropped when evicted
  and read from the ELF again; a written one goes to the swap file and comes from there
  after (PCSX2, which ignores D, writes every page out).
* With code in VM, a frame filled by the pager (zero fill, swap in, a move) is written out
  of the D-cache and the I-cache is dropped before it is mapped: every page starts at the
  same I-cache index, so the frame's lines of the page before would still hit.
* Keep core: interrupt handlers and what they read, thread stacks, what the DMAC follows,
  code that runs with interrupts off and may meet a page that is not resident.

### DMA and the IOP

* A physical address is a type of its own, `mx_paddr`, made only by `mx_paddr_of` (core or the
  scratchpad; VM ends the program with a report) and `mx_paddr_range(p, bytes)` (also VM pinned
  in one piece). `make check-phys` fails on hand-made physical addresses.
* `mx_vm_pin(p, bytes, flags)` makes pages resident and keeps them at their frames; with
  `MX_VM_PIN_DMA` in neighbouring frames (pages are moved there), so the range is one piece;
  with `MX_VM_PIN_WRITE` the pages count as dirty (the DMAC or the IOP writes them).
  `mx_vm_unpin_after(p, bytes, event)` unpins once a DMA queue's event has passed.
* Scatter-gather where maxsdk owns the tags: `mx_dma_send` (VIF0/VIF1/GIF/toIPU) and
  `mx_dma_to_spr` send pinned VM in several pieces as a chain of REF tags (TTE off);
  `mx_dma_from_spr` moves one piece at a time; `mx_fio_read`/`mx_fio_write` pin the buffer and
  call the file server once a piece.
* Everything that has its own tags (`mx_e_ref`, `mx_pkt_ref`, mesh chains, queue segments, the
  GS readback) takes VM only in one piece: one tag a REF always, so the sizes of chains do not
  depend on where the data is.

## Facts (PS2)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| `main` runs in user mode with COP0 usable: Status 0x70030c11 (KSU 2, CU0 1, EDI 1, IE and EIE 1); `tlbr`, `tlbwi`, `tlbp` work from it | | ps2link | `probe/p19_tlb.c` | 2026-10-31 |
| The BIOS's TLB: entry 0 the scratchpad, 1–12 the kernel, I/O (0x1000_0000 in 4 KiB pairs), VU and GS memory, ROM; 13–21 main memory cached, 22–30 uncached, 31–38 UCAB (not wired), 39–47 unused (kseg3, V = 0: entry *i* at 0xE000_0000 + *i* × 8 KiB, 0xE004_E000 for 39; PCSX2: (*i* − 1) × 8 KiB); Wired = 31. As ps2sdk's `InitTLB32MB` table. The VM parks the entries it clears above both (0xE000_0000 + (48 + *i*) × 8 KiB), so no two match one address | | | `probe/p19_tlb.c`, `tests/ps2/vm5_tlb.c` | 2026-11-01 |
| The page of timers 2 and 3 (0x1000_1000) is mapped without D: a user-mode store there raises TLB modified (code 1). `<mx/timer.h>`: use timers 0 and 1 | | | the first VM test's watchdog (since split into `tests/ps2/vm*_*.c`) | 2026-10-31 |
| The kernel's refill vector (0x8000_0000) and common vector (0x8000_0180) are the same code: the handler from a table at 0x8001_5340 by Cause.ExcCode, only k0 and k1 changed. `SetVTLBRefillHandler(1..3)` writes entries 1–3 of it; `SetVCommonHandler(1..3)` does not | | BIOS 2.00 | `probe/kdump.c`, `probe/p19_tlb.c` | 2026-10-31 |
| A TLB refill (no entry) reaches handler code 2 (load) or 3 (store), EntryHi = the faulting VPN2, BadVAddr set; the handler's `tlbwi` + `eret` continues the program | | | `probe/p19_tlb.c` | 2026-10-31 |
| A store to an entry without D reaches handler code 1 (TLB modified) | | | `probe/p19_tlb.c mod` | 2026-10-31 |
| **An access to an entry whose half is invalid (V = 0) hangs the PS2**: the handler (counting its calls) is not reached, no exception storm, no reset answer | 4 runs | BIOS 2.00 | `probe/p19_tlb.c inv` | 2026-10-31 |
| Refill through the kernel's dispatch + a 12-instruction handler, then the entry cleared from C | 116 cycles each | 64 runs | `probe/p19_tlb.c` | 2026-10-31 |
| Writing Status (KSU = 0) from user mode to get to kernel mode hung the PS2 (twice) | | | `probe/kdump.c` (first version) | 2026-10-31 |
| Kernel memory is read by a `SetSyscall` (0x74) handler: the kernel runs it in kernel mode | | | `probe/kdump.c` | 2026-10-31 |
| Negative syscall numbers (the `i…` calls): the kernel calls the handler at EXL = 1 on the caller's stack, below a 16-byte frame of its own (ra, EPC + 4); a handler that sets EPC and ERETs continues there (the frame left on the stack) | | | `probe/p19_tlb.c` | 2026-10-31 |
| The IOP file server's WRITE takes the data from EE memory itself (RDATA, `sceSifGetOtherData`, in its 16 KiB buffer's pieces); 300 B – 64 KiB in one call to `host:` are written right | | ps2link | `probe/fio_big.c` | 2026-10-31 |
| ps2link's tty answers WRITE with another count than the bytes: `mx_fio_write` checks the count for files only | | | `probe/fio_big.c` | 2026-10-31 |
| ps2client opened a `host:` file opened read-write (flags 3) write-only (`fix_flags`): reads gave 0 bytes. Fixed in our ps2client (`O_RDWR`) | | | `tests/ps2/vm2_host.c`, `tools/ps2client/tests` | 2026-10-31 |
| The kernel's SIF DMA queue was full when the SIF0 interrupt handler answered an RDATA request: `isceSifSetDma` returns 0. Dropped, the file server waits for good (no more fio, no tty); waiting in the handler hung the PS2. `src/base/sif.c` defers the answer to the waiting thread. Seen before the shared cache line below was found; not seen again since | | swap traffic | the first VM test | 2026-10-31 |
| **A SIF receive buffer that shares a cache line with a variable the CPU writes gets its answer overwritten** when that dirty line is written back: `wr_result` (16 bytes, the file server's answers) shared a line with the RPC clients, and a seek "returned" the previous write's 0 and the swap file stayed empty. Whether it strikes depends on the link layout and cache timing, so it came and went. Every buffer the IOP or the DMAC writes is now `MX_DMA_LINES` (`<mx/cpu.h>`: lines of its own) | | swap traffic | `tests/ps2/vm7_swap.c` | 2026-11-01 |
| A break instruction through the VM's vector and slow path (`mx__vm_break_init`): 352 breaks from two threads and with a timer interrupt, `host:` I/O in thread code in 350 of them; every GPR, an upper half, HI/LO and Status kept | | | `tests/ps2/vm4_exc.c` | 2026-11-01 |
| An interrupt handler's `iSignalSema` wakes a higher-priority thread at once, also while the main thread spins without a system call | 60 rounds of 32 KiB `host:` I/O | | `tests/ps2/vm3_defer.c` | 2026-11-01 |
| ps2link's file server answers CLOSE of a `host:` file with 3, not 0 (PCSX2: 0; ps2client sends 0) | | | `tests/ps2/vm2_host.c` | 2026-11-01 |
| A miss in a UCAB range once VM pages had replaced the BIOS's UCAB entries put back the wrong one: `alias_refill` took (PageMask \| 0x1FFF) + 1 for one page, while that is the size of the even/odd pair, so entry 31's range covered entry 32's too. The miss came again, and each time the same entry went into another slot: two TLB entries for one address. Fixed; `vm6_refill` step 7 reads through all eight | | | `tests/ps2/vm9_pins.c` (`mx__vm_check` found the size wrong) | 2026-11-01 |
| Another program's ps2client (e.g. `bench.sh run`, outside the runner's lock) cuts off `host:` files: a VM program with its swap file on `host:` then fails | | | | 2026-10-31 |

## Testing on the PS2

The VM is tested in steps, each its own ELF and each built on the ones before, so a failure
points at one part. Every step shows its progress on the debug screen (`mx_dbg_screen`,
`<mx/debug.h>`): the step as slot 0 and the background colour, a heartbeat from the vblank
interrupt in slots 30–31 (it stops when interrupts stay off), and at the end bright green with
900D0000 or bright red with BAD000nn. A photo of a hung screen says where it stopped.

| Step | Tests |
|---|---|
| `vm1_screen` | the debug screen itself: colours, words, interrupts off, a timer interrupt drawing |
| `vm2_host` | `host:` file I/O from the main thread: small, 32 KiB, unaligned, 200 random seeks, 128 KiB, reopen |
| `vm3_defer` | I/O asked for by a timer interrupt and done by a worker thread while the main thread spins |
| `vm4_exc` | exception → thread code → resume (a break through the VM's own vector and slow path), with I/O in the thread |
| `vm5_tlb` | TLB entries written by hand, no exceptions; they stay through interrupts, thread switches, I/O |
| `vm6_refill` | VM without swap: refills, zero fills in the thread, dirty tracking, a second thread, a timer reading a pinned page, reads through the BIOS's UCAB entries once VM pages have replaced them |
| `vm7_swap` | paging through `host:`: 32 frames for 96 pages, random touches, two threads faulting |
| `vm8_dma` | DMA from and to VM with every frame in use: scratchpad, VIF1 and GIF scatter-gather, `MX_VM_PIN_DMA` (the gather), queue unpin, fio |
| `vm9_pins` | 200 gathers of 1–4 pages at random places with every frame in use, the tables and the TLB checked after each (`mx__vm_check`: no two TLB entries match one address) |
| `vm10_mxload` | started by `mxload.elf` (`loader/mxload.c`), a physical program: argv (argv[0] its own path), .data, .rodata and a 256 KiB .bss, interrupts, `host:` I/O after the hand-over. Run as `mxload.elf host:vm10_mxload.elf a bc`; on PCSX2, which passes no arguments, they are in `host:mxload.txt` |
| `vm11_vmlink` | a program linked into VM (`MX_MEMORY=vm`, `ld/maxsdk-vm.ld`), started by mxload, with 16 frames and a swap file: its own code and data in VM, maxsdk and `MX_CORE_FN`/`MX_CORE_DATA` in core, core calling VM, four functions on pages of their own at one I-cache index called between rounds of heap writes (their frames reused for each other's code), a thread whose function is in VM |
| `vm12_elfpage` | a program linked into VM paged from its ELF: few pages read at `main`, a 128 KiB table and a 64 KiB array read when touched, evicted and read from the ELF again, half the array written and then from the swap file |

Steps 1–12 pass on the PS2 (2026-11-01) (`vm11_vmlink`'s first version, with 2650 swap
transfers in step 4, ran past the 60 s timeout: about 15 a second through `host:`); step 8 in 44 runs of 45, the last 44 in a row (with the
UCAB fix and the checks below). In the other one, the build before those, the file
server on the IOP stopped answering after the gather's swap writes: the debug screen showed
step 6 (the gather of step 5 done and its data right), the program in `wait_answer` with the
heartbeat running (interrupts on), each file call giving up after 12 s. Not understood yet;
the debug screen now counts those timeouts in slot 27 (`src/base/sif.c`, `fio_call`).

## Costs (PS2)

`probe/p20_vm.c`, 8 MiB of frames, swap file on `host:` (ps2link, Ethernet), 2026-11-01.

| What | Value |
|---|---|
| TLB refill of a resident page | 290 cycles (1024 refills) |
| First touch (zero fill, in the thread: the slow path and 32 KiB of stores) | 41522 cycles a page (64) |
| First store to a clean page (TLB modified) | 502 cycles |
| Page fault: swap in + write back of a dirty page | 11.5 ms (768 faults, 256 written back) |
| Page fault: swap in, clean page dropped | 16.7 ms (768 faults); the network's time, not the pager's |
| Reading 4 MiB resident VM vs core | 2903409 vs 2708231 cycles (VM 1.07 ×) |
| VIF1 64 KiB: VM pinned / VM in one piece (`MX_VM_PIN_DMA`) / core | 20598 / 20262 / 19677 cycles (the pinned range was one piece already) |

A swap file on `host:` costs milliseconds a fault: use VM for data that is touched rarely
(levels not in play, caches), not for what a frame reads.

## PCSX2

Run VM programs with `PCSX2_EE=interp` (the `ps2-pcsx2-test` runner): see `pcsx2.md`.
`mx_vm_init` returns -1 under the recompiler (it raises no TLB misses). Not with
`PCSX2_EE=interp-cache` either: with the data cache emulated, reads through VM give stale
memory after the TLB changes (`vm7_swap` crashes in step 2, `vm11_vmlink` reads a page it has
just loaded as zeros; both pass on the PS2), so the I-cache handling of code in VM can only be
tested on the PS2.
