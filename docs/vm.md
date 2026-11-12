# Virtual memory (`<mx/vm.h>`)

Optional: a program that calls `mx_vm_init` (or defines `mx_vm_config`) gets up to 128 MiB of
virtual memory at 0x0800_0000, in 32 KiB pages held in the physical memory left after the core
(the frames) and, when they run out, in a swap file. Without it nothing changes.

## Design

| Part | What |
|---|---|
| Core | The program (text, data, bss), the main stack, the VM's tables (the physical allocator, `mx__phys_alloc`, from the top of the heap). On the BIOS's fixed mappings, or, moved by the kernel (`kernel.md`, version 9), on entries of the program's ASID at a physical run of its own, the frames then the memory below it: it never faults. There is no core heap: everything else is frames. |
| Heaps (`<mx/mem.h>`) | Memory by kind, each heap its own: `MX_MEM_PINNED` and `MX_MEM_STREAM` are pinned memory, `MX_MEM_PAGED` and `MX_MEM_ASSET` pages of VM in the heap window (0x4800_0000, `mx_vm_config.heap_size`, apart from `malloc`'s VM), ASSET locked while DMA reads it (`mx_mem_lock_try`: no wait; the pager thread brings what is not resident). A heap's bookkeeping (a sorted table of extents) is apart from its memory: allocating and freeing touch none of its pages; freed whole pages of a heap of pages are dropped (no swap write). Without VM every kind is physical memory. |
| Pinned memory | Runs of frames taken out of the pool while in use (`pin_run`: from the top down, the pages in the way moved to free frames or written out), at one physical address, reached through `MX_PHYS_WIN` (0x6000_0000: all of main memory cached in one TLB entry, 16 MiB halves). `mx_paddr_of`, `MX_UNCACHED`, `MX_UCAB` work on it as on core memory, it costs no VM TLB entries, interrupt handlers may read it. A pinned heap grows by 64 KiB runs (an allocation larger than that: a run of its own) and gives a run back when nothing in it is used. DMA tags, chains, rings, thread stacks, interrupt handler data must be core or pinned. |
| VM | 0x0800_0000 – 0x0FFF_FFFF: not RAM, so a VM pointer taken for a physical address faults instead of reaching other memory. `malloc` (libmxc's `sbrk` → `mx_sbrk`) hands out VM when VM is on. |
| `malloc` | libmxc's own (`ee/libc/malloc.c`, TLSF: free blocks in lists by size class, found by bitmaps), not picolibc's: that one clears every block it hands out and walks one address-sorted free list on every `free`. Over VM both touch pages all over the heap, each touch maybe a swap transfer: Quake's 16 MiB hunk was written out to swap as it was allocated, and its loading thrashed (1700 page faults a second, no progress). Now `malloc` touches the block's header, its neighbours' and a list head; it does not clear (`calloc` does; new VM pages are zero) |
| Page | 32 KiB = one TLB entry, both 16 KiB halves valid (PageMask 16 KiB). An entry with an invalid half hangs the PS2 (below), so a page is in the TLB whole or not at all, and every miss is a refill. |
| TLB | `tlbwi` only, the VM's entries in turn (a ring, `next_of`, around the BIOS's kept ones), with a table of what each holds (no `tlbwr`, no `tlbp`: PCSX2 gets both wrong). `mx_vm_init` reads and saves all 48. The BIOS's entries for the scratchpad, kseg3, VU and GS memory, ROM, and the cached main memory that holds the BIOS's area (ps2link at 0x9_4000), the program, the physical allocator and the stack stay where they are (their code runs while the others change: no entry is moved). Its 8 pairs of 4 KiB I/O pages become one entry (64 KiB halves; the write protection of the timers 2/3 page and the SIF registers goes), its 9 uncached (0x2000_0000) and 8 UCAB (0x3000_0000) entries one each (16 MiB halves), its cached entries that hold only frames go, and one entry maps all main memory cached at `MX_PHYS_WIN`. About 31–34 entries for the VM (`mx_vm_stats`' `tlb_entries`), 1 MiB in the TLB at once. `mx_exit` and the crash report (`mx__vm_shutdown`) write the BIOS's entries and the TLB handlers back before returning to ps2link: the kept ones never changed, so the code doing it stays mapped. The VM's entries are not global (G = 0): they carry the ASID in `mx__asid` (the kernel gives a `libmxk` program its own, others 0), the fast refill takes it from EntryHi, which the refill exception loads with the missing page and the current ASID (EE Core User's Manual, EntryHi). `tlbr` and `tlbwi` change EntryHi, the CPU's ASID: both set it back. |
| Exception | Codes 1–3 (`SetVTLBRefillHandler`) go to `ee/src/vm/tlb.S`. A refill of a VM page that is mapped and has no entry is done there at once (the fast refill: three registers saved, the next entry written, the tables of `struct mx__vm_fast` in `vm_int.h` updated; not with a trace). Otherwise the registers are saved, C (`mx__vm_exception`, at EXL = 1, on its own stack, core memory only) does as little as it can: it refills the TLB, maps a resident page again, or marks the page dirty (TLB modified). Everything else goes to the thread. |
| Page fault | A new page (zero fill) or one in the swap file continues in the faulting thread: the registers are copied onto its stack, the exception returns into `mx__vm_slow` with the thread's interrupts, which takes the VM lock (a BIOS semaphore: other threads run meanwhile), zero-fills, or evicts and reads, then resumes by a negative syscall whose handler (at EXL = 1) restores everything and returns to the instruction. A fault with interrupts off, in the BIOS, or on a VM stack ends the program with a report: touch VM that may not be resident only from thread code with interrupts on (pin what an interrupt handler reads). |
| Eviction | LRU by refills: each refill stamps its page with the refill count (`struct mx__vm_fast`'s `stamp`), and at each vblank the VM's entries leave the TLB, so a page in use refills (is stamped) at least once a vblank (24 refills, some 4000 cycles). The page with the oldest stamp goes, written to its swap slot if dirty, dropped if clean; pinned and busy pages are skipped (a scan of the frames). In Quake III's trace (`tools/vmsim.py`), LRU paged about a third of what the clock (second chance, until 2026-11-01) did. |
| Dirty tracking | Clean pages are mapped without D: the first store raises TLB modified (code 1) and marks the page. `mx_vm_init` tests it: on PCSX2, which ignores D, every mapped page counts as dirty. |
| Swap file | Through the IOP file server (`host:`, `mass:`, …), opened read-write and truncated; slots are given out in order, so the file only grows at its end, and a page keeps its slot. A swap in is a seek and two 16 KiB READs, a write out a seek and one WRITE; a seek to where the file already is costs no call (`ee/src/base/sif.c` keeps each descriptor's position). The seek and the transfer hold a lock of their own (the writer thread and a faulting thread share the file). |
| Pager thread | `mx_mem_prefetch`, `mx_mem_lock_try` on a range not resident: a thread above the program's (at its priority − 1) brings the range in, so the program never waits for it (`<mx/vcache.h>`'s textures draw their pinned tail meanwhile). `mx_vm_init` moves the calling thread to priority 2 if it was above (0 when PCSX2 starts the program), so the VM's threads can run above it. |
| Writer thread | `mx_vm_config.clean_frames` (0: none): a thread above the priority of the one that called `mx_vm_init` keeps that many frames free or clean. It writes the dirty pages among those eviction takes next (the `clean_frames` oldest, less the free frames), pinned for the write; the page is clean from the start of the write, and a store meanwhile makes it dirty again (TLB modified; on PCSX2, which ignores D, the page is unmapped and dirty when mapped again). Faults then find clean victims and only read. It sleeps in its I/O (below), so the program runs meanwhile. |
| Waits for the IOP | A thread waiting for an answer (file server, audsrv) sleeps on its client's semaphore, so other threads run: woken by the answer, by an RDATA request to send, and at each vblank start (to give up after its time). With interrupts off or in a handler it spins. |
| DMA | The DMAC sees physical addresses only (`<mx/dma.h>`, below). |

### Programs linked into VM

* `MX_MEMORY=vm` (`mk/maxsdk.mk`) links with `ee/ld/maxsdk-vm.ld`: the program's own objects
  (text and rodata, then data and bss, each from a page) at 0x0800_0000, and the libraries'
  code (since 2026-11-12, mx OS plan 4b); crt0, the libraries' code that runs with interrupts
  off or before the VM is on (the objects listed in the script: libmxk's start-up, interrupt,
  SIF, DMA, GS, VU, debug and string code; picolibc's `mem*`, `str*`), the libraries' rodata,
  data and bss, libmxc's glue, the VU and HLSL objects, VU microcode, constructors, packets,
  `MX_DMA_LINES` buffers, `mx_vm_config`, and what the program marks `MX_CORE_FN` /
  `MX_CORE_DATA` stay core. `jal` reaches between the two. Each link of an MXOS program runs
  `tools/pinned-check.py`: what an `MX_CORE_FN` function (an interrupt handler) or code after
  a `di` calls must be core, else the link fails.
  A library linked as objects stays core when the program lists them in `MX_CORE_OBJS`
  (the link puts them in an archive of their own): Quake's `MX_CORE_OBJS = $(MXGL_OBJS)`.
* `MX_VM_TEXT=core` (with `MX_MEMORY=vm`, `ee/ld/maxsdk-vm-coretext.ld`): the program's code
  stays core too, only its rodata, data and bss are VM. The code then takes no VM TLB
  entries and is never paged, at the cost of as many frames. Quake III (2.2 MiB of code):
  12.0 fps with its code in VM, 12.6 in core; 14.0 against 3.8 million refills in 80 s of
  `timedemo four` (with the code in VM 27% of them were code, and its entries pushed the
  data's out of the TLB), and 591 against 671 swap ins (2.2 MiB fewer frames).
* Started by mxlink, the PS2's loader (`ps2max/apps/mxos`: `execee host:app.elf args...`; on
  PCSX2 its EMU build, `kernel.md`): it loads the core segments itself, leaves the VM ones, and
  starts the program in its own thread, argv[0] the program's path. The start-up turns VM on (`mx_vm_config`, or no swap
  file and every frame), opens argv[0] and registers the VM segments' pages with the pager
  (`ee/src/vm/image.c`), and starts the VM heap after them. Started otherwise, the program stops
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
  `mx_irq_dmac`, `mx_irq_intc`, `mx_timer_periodic`, `mx_sif_serve` (the handler and its
  argument) and a DMA queue's `on_done` end the program with a report when given VM
  (`mx__need_core`, `<mx/cpu.h>`); thread stacks already did (`mx_thread_start`). What the
  handler reads further on is not checked.

### Mapped files

* `mx_vm_map_file(path, &size)` maps a file read-only into the file window at 0x4000_0000
  (`MX_VM_MAP_BASE`, unmapped by the BIOS; `mx_vm_config.map_size`, 64 MiB by default):
  its pages follow VM's in the pager's tables, are read from the file when first touched,
  share the frames, and are never written: evicted, they are dropped and read again, so no
  swap file is needed and a level's data costs no swap traffic. A store into one ends the
  program with a report; a pin for writing is refused; pins for DMA work as for VM.
* Up to 8 files at once (with the program's ELF segments, 12 file entries). The window is
  given out from its start; unmapped space comes back once everything after it is unmapped.

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
* Assets in VM: `mx_vm_lock(p, bytes)` (a pin with `MX_VM_PIN_DMA`) keeps a range resident in
  one physical piece at fixed frames until `mx_vm_unlock`, so DMA tags and chains built at
  load may point into it: `mx_paddr_of` and `mx_paddr_range` give pinned VM's frame (VM not
  pinned is still a report). Lock what an area or level uses while it is loaded; unlocked,
  its pages may be evicted (a mapped file's dropped, others to the swap file), and locked
  again they may be at other frames: chains built for them are built again.
  `mx_frame_unlock` (`<mx/frame.h>`) unlocks once the frame being built is done. A range of a
  mapped file can be locked and sent as it is (read-only: data the GS or VU1 takes as
  stored), which costs no swap traffic; the asset file must keep DMA data 16-byte aligned.
* `MX_VM_PIN_OVERWRITE` (with `MX_VM_PIN_WRITE`): pages that lie whole in the range and are
  not resident get a frame without their contents: no swap file read, file read or zero fill
  for what the transfer overwrites anyway. `mx_fio_read` into VM pins so; when the read is
  short (the end of the file), `mx_vm_overwrite_done` reads the rest of those pages back
  through a frame taken for the time (zero for a page never written), so the bytes past the
  read stay as they were. Stats: `overwrites`, `restores`.
* Everything that has its own tags (`mx_e_ref`, `mx_pkt_ref`, mesh chains, queue segments, the
  GS readback) takes VM only in one piece: one tag a REF always, so the sizes of chains do not
  depend on where the data is.

### The trace

`mx_vm_config.trace` (bytes, core) records what a program touches, for `tools/vmsim.py`,
which replays it with other page sizes, memory sizes and eviction policies:
* The TLB then maps 8 KiB blocks (PageMask 4 KiB, 4 entries a page), so a refill says which
  block was touched. The pager is unchanged (32 KiB pages, the same frames and eviction);
  the TLB holds 192 KiB instead of 768 KiB, so there are more refills.
* Each interval (`trace_ms`, 100 ms; checked at each vblank) begins with a tick and the VM's
  entries out of the TLB: the first touch of each block in an interval is a refill and is
  recorded, read or written. Entries are written without D until a store to their block is
  seen in that interval (on PCSX2, which ignores D, only a store that refills counts as one).
  A page the pager unmaps (evicted) is recorded again when
  touched again in the same interval. Ranges pinned (`mx_vm_pin`: the DMAC's, the IOP's
  file transfers) are recorded as touched.
* The trace is a u32 per event (`struct mx_vm_trace_head`, `MX_VM_TR_*`): blocks read or
  written, ticks (ms), and marks (`mx_vm_trace_mark`). `mx_vm_trace_restart` drops what was
  recorded (a game's loading), `mx_vm_trace_save(path)` writes it. When the buffer is full,
  recording stops (the head's `lost`).
* `tools/vmsim.py <trace> --window A B` replays it (from its start, so memory is warm) and
  counts between marks A and B: the working set of an interval at piece sizes of 8–256 KiB,
  the faults, swap ins and outs of clock, LRU and OPT (Belady's: the fewest faults any policy
  can have) at 8, 16 and 32 KiB pages in the same memory, their time by the costs below, and
  how many of a 32 KiB page's blocks were touched and written while it was resident.
* Replaying `vm18_trace` (PCSX2) with the clock at 32 KiB gives the run's swap ins exactly
  (72); the writes differ, since PCSX2 writes every page out (no dirty tracking).

## Facts (PS2)

| Fact | Value | Conditions | Source | Date |
|---|---|---|---|---|
| `ei` needs a `sync.p` after it: without it a Status read a few instructions later (the next `mx_di`) could still see EIE clear, so that `mx_di` returned 0 and its `mx_irq_restore` left interrupts off; the next page fault for the thread then ended the program ("interrupts off"). `mx_ei` has it | | | `tests/ps2/vm6_refill.c`, `probe/p22_refill.c` | 2026-11-01 |
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
| The kernel's SIF DMA queue was full when the SIF0 interrupt handler answered an RDATA request: `isceSifSetDma` returns 0. Dropped, the file server waits for good (no more fio, no tty); waiting in the handler hung the PS2. `ee/src/base/sif.c` defers the answer to the waiting thread. Seen before the shared cache line below was found; not seen again since | | swap traffic | the first VM test | 2026-10-31 |
| **A SIF receive buffer that shares a cache line with a variable the CPU writes gets its answer overwritten** when that dirty line is written back: `wr_result` (16 bytes, the file server's answers) shared a line with the RPC clients, and a seek "returned" the previous write's 0 and the swap file stayed empty. Whether it strikes depends on the link layout and cache timing, so it came and went. Every buffer the IOP or the DMAC writes is now `MX_DMA_LINES` (`<mx/cpu.h>`: lines of its own) | | swap traffic | `tests/ps2/vm7_swap.c` | 2026-11-01 |
| A break instruction through the VM's vector and slow path (`mx__vm_break_init`): 352 breaks from two threads and with a timer interrupt, `host:` I/O in thread code in 350 of them; every GPR, an upper half, HI/LO and Status kept | | | `tests/ps2/vm4_exc.c` | 2026-11-01 |
| An interrupt handler's `iSignalSema` wakes a higher-priority thread at once, also while the main thread spins without a system call | 60 rounds of 32 KiB `host:` I/O | | `tests/ps2/vm3_defer.c` | 2026-11-01 |
| The trimmed TLB (I/O as one 64 KiB pair, uncached and UCAB 32 MiB each, `MX_PHYS_WIN`, the frames' cached entries gone): `vm1`–`vm20` pass, `mx__vm_check` finds no two entries for one address; 31 VM entries for a small program, 34 for Quake III | | PCSX2 only (EE interpreter); not yet on the PS2 | `tests/ps2/vm20_heaps.c`, `vm6_refill.c` | 2026-11-03 |
| PCSX2 starts a program at thread priority 0: the VM's threads (priority − 1) could not run above it until `mx_vm_init` moved it to 2 (the pager thread waited for the program to block) | | PCSX2 | `tests/ps2/vm20_heaps.c` step 4 | 2026-11-03 |
| ps2link's file server answers CLOSE of a `host:` file with 3, not 0 (PCSX2: 0; ps2client sends 0) | | | `tests/ps2/vm2_host.c` | 2026-11-01 |
| A miss in a UCAB range once VM pages had replaced the BIOS's UCAB entries put back the wrong one: `alias_refill` took (PageMask \| 0x1FFF) + 1 for one page, while that is the size of the even/odd pair, so entry 31's range covered entry 32's too. The miss came again, and each time the same entry went into another slot: two TLB entries for one address. Fixed; `vm6_refill` step 7 reads through all eight | | | `tests/ps2/vm9_pins.c` (`mx__vm_check` found the size wrong) | 2026-11-01 |
| A program linked into VM that opened a path in its `.rodata` (VM, read from its ELF when first touched) hung for good: the file call took the kernel's file lock, read the path, faulted, and the pager's read of the page from the ELF waited for the lock the same thread held. `mx_fio_open`, `mx_fio_dopen`, remove, mkdir and rmdir copy the path into core (the stack) before the lock, `mx_fio_dread` reads the name into core and copies it out after; reads and writes pin their buffer before the lock already. A first `mx_printf` of a string on that page had hidden it | | | `tests/ps2/k44_read.c` linked into VM, PCSX2 and PS2 | 2026-11-11 |
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
| `vm6_refill` | VM without swap: refills, zero fills in the thread, dirty tracking, a second thread, a timer reading a pinned page, reads through the UCAB ranges (the BIOS's 8 entries, now one) |
| `vm7_swap` | paging through `host:`: 32 frames for 96 pages, random touches, two threads faulting |
| `vm8_dma` | DMA from and to VM with every frame in use: scratchpad, VIF1 and GIF scatter-gather, `MX_VM_PIN_DMA` (the gather), queue unpin, fio |
| `vm9_pins` | 200 gathers of 1–4 pages at random places with every frame in use, the tables and the TLB checked after each (`mx__vm_check`: no two TLB entries match one address) |
| `vm11_vmlink` | a program linked into VM (`MX_MEMORY=vm`, `ee/ld/maxsdk-vm.ld`), started by mxlink, with 16 frames and a swap file: its own code and data in VM, maxsdk and `MX_CORE_FN`/`MX_CORE_DATA` in core, core calling VM, four functions on pages of their own at one I-cache index called between rounds of heap writes (their frames reused for each other's code), a thread whose function is in VM |
| `vm12_elfpage` | a program linked into VM paged from its ELF: few pages read at `main`, a 128 KiB table and a 64 KiB array read when touched, evicted and read from the ELF again, half the array written and then from the swap file |
| `vm13_guards` | in a program linked into VM, `MX_CORE_FN` handlers with `MX_CORE_DATA` data run (a timer, the vblank start); each guard refuses a VM handler or argument (with a test hook instead of the report): timer, INTC, DMAC, SIF server, a DMA queue's `on_done` |
| `vm14_mapfile` | a 300 KiB file mapped with 8 frames and no swap file: in the file window, read twice (read again, dropped, never written out), random reads, a DMA pin and a VIF1 transfer of mapped data, a write pin refused, `mx_fio_write` from the mapping, two files mapped, unmapped and mapped again at the same places. `vm14_mapfile.elf store`: a store into the mapping, which ends the program (status 255) |
| `vm15_overwrite` | `mx_fio_read` into VM pinned with `MX_VM_PIN_OVERWRITE`, 8 frames: a read at byte 100 over 5 pages in the swap file (4 not read in, the bytes around as they were), a short read over 4 pages (the rest of 2 restored from the swap file), reads into new pages (no zero fill; past a short read, zero) |
| `vm16_lock` | in a program linked into VM: a static buffer locked and sent through `mx_paddr_of`; 3 pages of a mapped file locked, sent from 3 places, unmoved by 40 pages of heap writes; unlocked and dropped, locked again from the file; `mx_frame_unlock` holds the lock until the frame that REFs the range is done |
| `vm17_writer` | the writer thread (`clean_frames` 4, 16 frames): 400 random whole-page writes and checks of 48 pages and a second thread, the tables checked every 50; most write outs by the writer, most evictions drops; 100 KiB of pinned memory out of the pool and back |
| `vm19_undo` | an `mx_vm_init` that fails (too few frames, no memory for the trace, no TLB misses) gives back the physical allocator's tables and the break |
| `vm20_heaps` | the trimmed TLB (30 entries or more, no two for one address, `MX_PHYS_WIN` reads what the BIOS's mapping reads); 40 small pinned allocations in 64 KiB runs, written through `MX_UCAB`; 512 KiB pinned while 3 MiB of pages churn (pages moved and written out), sent by DMA; an asset heap's 64 KiB locked, swapped out, `mx_mem_lock_try` -1 until the pager brought it in; `mx_heap_paged`, `mx_dma_alloc` |
| `vm21_crash` | in a program linked into VM: a break, and with `note` a store outside the VM in use: mx-kernel reports it, with the VM's note, and keeps the report. Ends with status 255 |
| `vm18_trace` | the trace (8 frames, 64 KiB, 100 ms): known touches between marks recorded once an interval, read or written; 24 pages written and checked twice with 8 KiB TLB entries, `mx__vm_check`; the trace saved, read back and checked |

Steps 1–15 pass on the PS2 (2026-11-01), step 16 on PCSX2 (not run on the PS2 yet), and Quake1 linked into VM (`MX_MEMORY=vm`, 600 demo frames at 53–72 fps) (`vm11_vmlink`'s first version, with 2650 swap
transfers in step 4, ran past the 60 s timeout: about 15 a second through `host:`); step 8 in 44 runs of 45, the last 44 in a row (with the
UCAB fix and the checks below). In the other one, the build before those, the file
server on the IOP stopped answering after the gather's swap writes: the debug screen showed
step 6 (the gather of step 5 done and its data right), the program in `wait_answer` with the
heartbeat running (interrupts on), each file call giving up after 12 s. Not understood yet;
the debug screen now counts those timeouts in slot 27 (`ee/src/base/sif.c`, `fio_call`).

## Costs (PS2)

`probe/p20_vm.c`, 8 MiB of frames, swap file on `host:` (ps2link, Ethernet), 2026-11-01.

| What | Value |
|---|---|
| TLB refill of a resident page | 176 cycles with the fast refill, 470 through the full save and C (`probe/p22_refill.c`; `probe/p20_vm.c` measured 290 for the latter) |
| First touch (zero fill, in the thread: the slow path and 32 KiB of stores) | 41522 cycles a page (64) |
| First store to a clean page (TLB modified) | 502 cycles |
| Page fault: swap in + write back of a dirty page | 11.5 ms (768 faults, 256 written back) |
| Page fault: swap in, clean page dropped | 16.7 ms (768 faults); the network's time, not the pager's |
| Reading 4 MiB resident VM vs core | 2903409 vs 2708231 cycles (VM 1.07 ×) |
| VIF1 64 KiB: VM pinned / VM in one piece (`MX_VM_PIN_DMA`) / core | 20598 / 20262 / 19677 cycles (the pinned range was one piece already) |

What one call of the IOP costs (`probe/p21_iolat.c`, swap file on `host:`, 2026-11-01; the
same with waits that spin, and 3–5% more while a sound stream plays):

| Call | Time |
|---|---|
| seek | 1.0 ms |
| READ 16 KiB / 32 KiB (one call: ps2link takes it) | 7.8 / 15.5 ms (about 2 MB/s) |
| WRITE 32 KiB | 40.5 ms (about 0.8 MB/s) |
| audsrv room (`mx_snd_stream_room`, estimated, no call) | 4 µs |

So a swap in costs about 17 ms and a write out about 41 ms: a fault that evicts a dirty page
about 58 ms. Writes are the expensive part; the writer thread (`clean_frames`) takes them out
of the faults when the IOP has time between them (it does one transfer at a time: a program
that pages without pause, such as `vm17_writer` on the PS2, keeps it busy and the writer
cannot get ahead).

A swap file on `host:` costs milliseconds a fault: use VM for data that is touched rarely
(levels not in play, caches), not for what a frame reads. Data a frame reads (sound
samples being mixed, the level in play) can be kept resident with `mx_vm_pin` or
`mx_vm_lock`: each pinned page is a frame taken from the pager for as long. Pager time
shows in `mx_vm_stats` (`us_swap_in`, `us_swap_out`, `us_file_in`, `us_lock`, `io_calls`,
`writer_outs`); `probe/p21_iolat.c` measures what each call of the IOP costs. With VM on, the
end marker (`MX_TEST_END`) and `mx_exit` print a line of the pager's totals and one of the
IOP's waits (all, and the file server's), and `mx_prof_report` what changed between
`mx_prof_start` and `mx_prof_stop`: the profile of a game's frames then says how much of
its waiting is the pager.

Eviction does not prefer clean pages: a program's code in VM is clean (read from its ELF)
and among the hottest; with clean pages taken first (with the clock), Quake III's loading
went to frames of 10 s on the PS2. The writer thread instead cleans the dirty pages
eviction takes next.

The IOP's file manager has 16 descriptors, 3 of them for stdio; a program linked into VM
holds 2 (its ELF and the swap file), and each mapped file one.

## PCSX2

Run VM programs with `PCSX2_EE=interp` (the `ps2-pcsx2-test` runner): see `pcsx2.md`.
`mx_vm_init` returns -1 under the recompiler (it raises no TLB misses). Not with
`PCSX2_EE=interp-cache` either: with the data cache emulated, reads through VM give stale
memory after the TLB changes (`vm7_swap` crashes in step 2, `vm11_vmlink` reads a page it has
just loaded as zeros; both pass on the PS2), so the I-cache handling of code in VM can only be
tested on the PS2.
