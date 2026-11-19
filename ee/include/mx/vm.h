#ifndef MX_VM_H
#define MX_VM_H

/* Virtual memory (optional): up to 128 MiB at MX_VM_BASE, in 32 KiB pages
   held in the physical memory left after the core (frames) and, when they
   run out, in a swap file. Off unless the program asks for it.

   The core is never paged: the program (text, data, bss), the main stack
   and maxsdk's own tables. It stays on the BIOS's fixed mappings, so it
   never faults. Everything else is frames: pages of VM, and pinned memory
   (<mx/mem.h>: runs of frames taken out of the pool while in use, at
   MX_PHYS_WIN). Interrupt handlers, BIOS calls, thread stacks and the data
   the DMAC follows (tags, chains, rings) must be core or pinned.

   The TLB: the BIOS's entries for the program, its stack, ps2link and the
   kernel stay; its I/O, uncached and UCAB entries are merged, those of
   main memory that hold only frames go, so the VM has about 34 entries of
   the 48 (mx_vm_stats: tlb_entries). mx_exit and the crash report write
   the BIOS's TLB and TLB handlers back before returning to ps2link.

   With VM on, malloc (libmxc's sbrk) hands out VM. A VM access from code
   with interrupts off (an interrupt handler, under mx_di) or from the BIOS
   may only touch resident pages (or a new page while frames are free):
   anything that needs the swap file ends the program with a report.

   The DMAC and the IOP see physical memory only: see <mx/dma.h> and
   mx_vm_pin. Page faults are handled in docs/vm.md order: the TLB refill
   and first-touch zero fill in the exception, swap in the faulting thread
   (other threads run while it waits). */

#include <mx/cpu.h>
#include <mx/dma.h>
#include <mx/types.h>

/* One TLB entry, both halves: the PS2 hangs on an entry with an invalid
   half (docs/vm.md). */
#define MX_VM_PAGE 32768u

struct mx_vm_config {
  char swap[64]; /* swap file ("host:swap.bin", "mass:/swap.bin"); "": none
                    (then VM is as large as the frames). In the struct, not a
                    pointer: a program linked into VM reads it before VM is on */
  u32 size;      /* bytes of VM; 0: 128 MiB */
  u32 reserved0; /* 0 (it was the heap window's size: the region has none to set) */
  u32 frames;    /* bytes of physical memory for pages; 0: the rest */
  u32 reserved1; /* 0 (it was the file window's size) */
  u32 clean_frames; /* 0, or a writer thread keeps that many frames free or
                       clean (their pages written to the swap file ahead),
                       so a page fault reads and does not write first. It
                       runs above the priority of the thread that calls
                       mx_vm_init and sleeps in its I/O */
  u32 trace;    /* 0, or bytes of a trace of what the program touches, for
                   tools/vmsim.py (mx_vm_trace_save): core memory, so the
                   frames are fewer by as much. Slower:
                   the TLB then maps 8 KiB pieces */
  u32 trace_ms; /* the trace's interval; 0: 100 ms */
};

/* A program that defines this gets VM before its static constructors run
   (so malloc is in VM from the start). */
extern const struct mx_vm_config mx_vm_config
    __attribute__((weak, section(".rodata.mx.irq"))); /* the core's interrupt part: read before VM is on */

/* Turn VM on. Returns 0, or -1 (VM stays off: no swap file, too little
   memory, already on). */
int mx_vm_init(const struct mx_vm_config *cfg);
/* (The calling thread goes to priority 2 if it was above it, so the VM's
   writer and pager threads, at its priority - 1, can run above it.) */
int mx_vm_active(void);
MX_INLINE void *mx_vm_base(void) { return (void *)(uintptr_t)MX_VM_BASE; }
u32 mx_vm_size(void);

/* ---- mapped files ---------------------------------------------------------------- */

/* Map file path, read-only, into a free piece of the region
   (MX_VM_REGION_BASE, 0x4000_0000, 512 MiB for mapped files and heaps of
   pages together; a piece is whole 2 MiB): its pages are read when first
   touched and share the frames with VM, but are never written: evicted,
   they are dropped and read again (no swap file needed). A store into one
   ends the program with a report. The file stays open until unmapped; its
   piece is free again then. Up to 60 files. Thread code. Returns the
   address (*size the file's bytes), or 0. */
const void *mx_vm_map_file(const char *path, u32 *size);
void mx_vm_unmap_file(const void *p);
/* The same with a choice (kernel version 36). MX_VM_MAP_RESIDENT: the whole
   file is read at the call into pinned memory, in one physical piece: no
   page faults and no TLB refills when it is read, the DMAC can read it
   (mx_paddr_of, MX_UNCACHED and MX_UCAB work on it), and it takes its size
   out of the frames the rest of the program pages with, until it is
   unmapped. For data read all over every frame; the paged form is for
   data read now and then. Up to 8 such files. 0: no run of frames that
   long, or as mx_vm_map_file. flags 0: mx_vm_map_file. */
#define MX_VM_MAP_RESIDENT 1u
const void *mx_vm_map_file_flags(const char *path, u32 *size, u32 flags);

/* ---- DMA and the IOP --------------------------------------------------------- */

/* Pin [p, p + bytes) for the DMAC or the IOP: its pages are made resident
   and stay so, at the same frames, until unpinned (pins count). From a
   thread only (it may wait for the swap file). Returns 0, or -1 (not VM,
   or MX_VM_PIN_DMA and no run of frames that long). Core memory: 0,
   nothing to do. */
#define MX_VM_PIN_WRITE 1 /* the DMAC or the IOP writes the memory */
#define MX_VM_PIN_DMA 2   /* in one physical piece (mx_paddr_range works on it) */
/* every byte will be written (with MX_VM_PIN_WRITE): pages that lie whole
   in the range and are not resident get a frame without their contents (no
   swap file read, no file read, no zero fill). If the transfer writes less,
   mx_vm_overwrite_done puts the rest back; otherwise it is lost. Not with
   MX_VM_PIN_DMA (those pages are read in). mx_fio_read uses it. */
#define MX_VM_PIN_OVERWRITE 4
int mx_vm_pin(const void *p, u32 bytes, u32 flags);
void mx_vm_unpin(const void *p, u32 bytes);
/* Lock [p, p + bytes) for DMA while it is in use (an asset: a texture, a
   model's streams, a range of a mapped file): resident, in one physical
   piece, kept at its frames until unlocked, so DMA tags and chains may
   point into it (mx_paddr_of, mx_paddr_range, mx_e_ref). A pin with
   MX_VM_PIN_DMA; locks count. Returns 0, or -1 (no run of frames that
   long: unlock something, or split the asset). Unlocked, the pages may be
   evicted (a mapped file's dropped, others to the swap file); locked again
   they may be at other frames, so chains built for them are built again.
   A frame in flight may still read a range: mx_frame_unlock (<mx/frame.h>)
   unlocks it once the frame is done. */
MX_INLINE int mx_vm_lock(const void *p, u32 bytes) { return mx_vm_pin(p, bytes, MX_VM_PIN_DMA); }
MX_INLINE void mx_vm_unlock(const void *p, u32 bytes) { mx_vm_unpin(p, bytes); }
/* Before mx_vm_unpin, after a transfer into [p, p + bytes) pinned with
   MX_VM_PIN_OVERWRITE that wrote only its first done bytes: the rest of the
   pages it did not read in gets their contents back. */
void mx_vm_overwrite_done(const void *p, u32 bytes, u32 done);
/* Unpin once event e of a DMA queue has passed (the transfer that reads or
   writes the range is done): checked by the VM before it evicts, and by
   mx_vm_unpin_done. */
void mx_vm_unpin_after(const void *p, u32 bytes, mx_event e);
/* Unpin the ranges whose events have passed; returns how many are left. */
u32 mx_vm_unpin_done(void);

/* The physical pieces of [p, p + bytes), pinned VM (or core memory: one
   piece), neighbouring frames merged. Returns their number; more than max,
   or a page not pinned, ends the program with a report. */
struct mx_vm_piece {
  mx_paddr addr;
  u32 bytes;
};
u32 mx_vm_pieces(const void *p, u32 bytes, struct mx_vm_piece *pc, u32 max);
/* The first of them (at most bytes long). */
struct mx_vm_piece mx_vm_first_piece(const void *p, u32 bytes);

/* ---- statistics ---------------------------------------------------------------- */

struct mx_vm_stats {
  u32 pages, frames, free_frames, pinned;
  u32 pinned_frames; /* frames out of the pool as pinned memory (<mx/mem.h>) */
  u32 tlb_entries;   /* TLB entries the VM has (the BIOS's kept: the rest) */
  u32 refills;    /* TLB refills (no entry): MX_VM_REFILL_CYCLES each */
  u32 refills_i;  /* of them, instruction fetches (code in VM) */
  u32 zero_fills; /* first touches */
  u32 faults;     /* page faults handled in the thread: new pages, the swap file */
  u32 swap_ins, swap_outs, drops; /* drops: clean pages evicted without I/O */
  u32 file_ins;   /* pages read from files (a program linked into VM, mapped files) */
  u32 overwrites; /* pages pinned to be overwritten: not read in (MX_VM_PIN_OVERWRITE) */
  u32 restores;   /* of those, written short and read in after (mx_vm_overwrite_done) */
  /* time (µs) the pager spent in the file server, by what for, and its calls */
  u32 us_swap_in, us_swap_out, us_file_in;
  u32 us_lock;  /* waiting for the VM lock (another thread's fault or pin) */
  u32 io_calls; /* file server calls of the pager (seeks, reads, writes) */
  u32 writer_outs; /* pages the writer thread wrote ahead (clean_frames) */
  u32 retries;                    /* swap I/O repeated (the file server did not answer) */
  u32 dirty_tracking;             /* 1: TLB modified exceptions (the PS2), 0: every
                                     mapped page counts as written (PCSX2) */
};
void mx_vm_stats(struct mx_vm_stats *s);

/* Where the program's TLB refills fall (kernel version 35): a count for each
   32 KiB page of its VM and of what it has of the region. The first call starts the
   counting and returns 0. Each later call writes up to max pages that had
   refills since the call before (their address and count, by address) and
   starts those at 0 again; it returns how many it wrote (as many as max:
   call again for the rest). -1: no VM, a trace is on, no memory for the
   table (a word a page), or a kernel before version 35. For measuring: a
   page touched in a frame refills at least once a frame (the VM's entries
   leave the TLB at each vblank), so the pages with the highest counts are
   the ones to bring together or to keep out of VM. */
struct mx_vm_hot {
  u32 va;
  u32 count;
};
int mx_vm_refill_hist(struct mx_vm_hot *out, int max);
/* Write the program's written pages to the swap file now (kernel version
   43): every page it wrote that is resident and not pinned goes to the swap
   file and stays where it is, so that when eviction takes it later it is
   dropped instead of written (a write of a page: about 7 ms on a memory card
   or USB, a whole field of a frame). For a point where a wait is fine,
   such as the end of a level's load: the pages it wrote are cold then and
   would otherwise be written one by one as the level runs. It takes a
   write a page (seconds for thousands of pages); the program's other
   threads fault between them. A page written again afterwards is dirty
   again. Returns the pages written; -1 without VM, without a swap file, or
   a kernel before version 43. Counted in mx_vm_stats' swap_outs. */
int mx_vm_clean(void);
/* What a TLB refill of a resident page costs (the PS2, tlb.S's fast
   refill, probe/p22_refill.c; docs/vm.md "Costs") */
#define MX_VM_REFILL_CYCLES 176

/* ---- the trace (mx_vm_config.trace) ------------------------------------------------ */

/* With a trace, the TLB maps the pages in 8 KiB pieces ("blocks", 4 a
   page), and each interval (trace_ms, at a vblank) its VM entries are
   cleared, so the first touch of each block in each interval is a refill:
   recorded once an interval as read or written (a store, or the first one
   to it: TLB modified). The pager runs as without a trace (the same pages,
   frames and eviction); only the TLB holds less (192 KiB). Recording stops
   when the buffer is full. */
#define MX_VM_TRACE_MAGIC 0x5456584du /* "MXVT" */
struct mx_vm_trace_head {
  u32 magic, version;       /* MX_VM_TRACE_MAGIC, 1 */
  u32 page, block;          /* bytes: MX_VM_PAGE, 8192 */
  u32 npages, nmap, nframes; /* pages of the VM window (4096: block numbers of
                                the region follow those), of the region, frames */
  u32 interval_ms, events, lost;
};
/* Events, a u32 each: kind << 30 | value. */
#define MX_VM_TR_READ 0  /* block (page * 4 + piece) read in this interval */
#define MX_VM_TR_WRITE 1 /* block written in this interval (read too, maybe) */
#define MX_VM_TR_MARK 2  /* mx_vm_trace_mark's tag (30 bits) */
#define MX_VM_TR_TICK 3  /* an interval begins: ms since the trace began */
/* Without a trace (mx_vm_config.trace 0; always under the kernel):
   mx_vm_trace_restart starts a fault log of the calling program, or starts
   it again: each page fault that made a page resident, 4096 at most, in
   64 KiB of frames the program gives up until it ends. One program at a
   time (another's restart does nothing). The file: the head (version 2,
   block 0, interval_ms 0), then events of 16 bytes. Thread code. */
struct mx_vm_fault {
  u32 va_flags; /* the page's address | MX_VM_FAULT_* */
  u32 epc;      /* the faulting instruction (0: a pin or the pager); a mark: its tag */
  u32 t_us;     /* since the restart */
  u32 us;       /* what the fault took (the room made, the read), not the lock's wait */
};
#define MX_VM_FAULT_KIND 3u    /* va_flags & 3: */
#define MX_VM_FAULT_ZERO 0u    /*   a new page, zero filled */
#define MX_VM_FAULT_SWAP 1u    /*   read from the swap file */
#define MX_VM_FAULT_FILE 2u    /*   read from a file (code and data of a program linked into VM, a mapped file) */
#define MX_VM_FAULT_MARK 3u    /*   mx_vm_trace_mark (va_flags is 3, epc the tag) */
#define MX_VM_FAULT_WRITE 4u   /* a store */
#define MX_VM_FAULT_OUT 8u     /* a dirty page was written to the swap file first, for room */
#define MX_VM_FAULT_PIN 16u    /* by mx_vm_pin, not an instruction */
#define MX_VM_FAULT_PAGER 32u  /* the pager's read ahead (mx__vm_prefetch) */
/* A mark in the trace (a frame, a level, a profile's start and stop). */
void mx_vm_trace_mark(u32 tag);
/* Drop what was recorded and record from here (a game's loading left
   out: a few seconds before the frames to study, so memory is warm when
   they start). */
void mx_vm_trace_restart(void);
/* Write the trace (or the fault log) to path (the head, then the events),
   and go on recording. Thread code. Returns the events written, or -1. */
int mx_vm_trace_save(const char *path);

/* The program break (libmxc's sbrk): VM when VM is on, otherwise the core
   heap's other end. Returns the old break, or (void *)-1. */
void *mx_sbrk(s32 incr);

#endif
