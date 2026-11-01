#ifndef MX_VM_H
#define MX_VM_H

/* Virtual memory (optional): up to 128 MiB at MX_VM_BASE, in 32 KiB pages
   held in the physical memory left after the core (frames) and, when they
   run out, in a swap file. Off unless the program asks for it.

   The core is never paged: the program (text, data, bss), the main stack,
   the core heap (mx_core_alloc), and maxsdk's own tables. It stays on the
   BIOS's fixed mappings, so it never faults. Interrupt handlers, BIOS calls,
   thread stacks and the data the DMAC follows (tags, chains, rings) must be
   core.

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
  u32 core_heap; /* bytes kept for mx_core_alloc; 0: 1 MiB */
  u32 frames;    /* bytes of physical memory for pages; 0: the rest */
  u32 map_size;  /* bytes of the file window (mx_vm_map_file); 0: 64 MiB */
  u32 clean_frames; /* 0, or a writer thread keeps that many frames free or
                       clean (their pages written to the swap file ahead),
                       so a page fault reads and does not write first. It
                       runs above the priority of the thread that calls
                       mx_vm_init and sleeps in its I/O */
  u32 trace;    /* 0, or bytes of a trace of what the program touches, for
                   tools/vmsim.py (mx_vm_trace_save): core memory besides
                   core_heap, so the frames are fewer by as much. Slower:
                   the TLB then maps 8 KiB pieces */
  u32 trace_ms; /* the trace's interval; 0: 100 ms */
};

/* A program that defines this gets VM before its static constructors run
   (so malloc is in VM from the start). */
extern const struct mx_vm_config mx_vm_config
    __attribute__((weak, section(".rodata.mx.core"))); /* core, also in a program linked into VM */

/* Turn VM on. Returns 0, or -1 (VM stays off: no swap file, too little
   memory, already on). */
int mx_vm_init(const struct mx_vm_config *cfg);
int mx_vm_active(void);
MX_INLINE void *mx_vm_base(void) { return (void *)(uintptr_t)MX_VM_BASE; }
u32 mx_vm_size(void);

/* ---- mapped files ---------------------------------------------------------------- */

/* Map file path, read-only, into the file window (MX_VM_MAP_BASE,
   0x4000_0000, mx_vm_config.map_size): its pages are read when first
   touched and share the frames with VM, but are never written: evicted,
   they are dropped and read again (no swap file needed). A store into one
   ends the program with a report. The file stays open until unmapped. Up
   to 8 files; the window is given out from its start, and space comes
   back when the last mapping goes. Thread code. Returns the address (*size
   the file's bytes), or 0. */
const void *mx_vm_map_file(const char *path, u32 *size);
void mx_vm_unmap_file(const void *p);

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
  u32 npages, nmap, nframes; /* VM pages, the file window's, frames */
  u32 interval_ms, events, lost;
};
/* Events, a u32 each: kind << 30 | value. */
#define MX_VM_TR_READ 0  /* block (page * 4 + piece) read in this interval */
#define MX_VM_TR_WRITE 1 /* block written in this interval (read too, maybe) */
#define MX_VM_TR_MARK 2  /* mx_vm_trace_mark's tag (30 bits) */
#define MX_VM_TR_TICK 3  /* an interval begins: ms since the trace began */
/* A mark in the trace (a frame, a level, a profile's start and stop). */
void mx_vm_trace_mark(u32 tag);
/* Drop what was recorded and record from here (a game's loading left
   out: a few seconds before the frames to study, so memory is warm when
   they start). */
void mx_vm_trace_restart(void);
/* Write the trace to path (the head, then the events), and go on
   recording. Thread code. Returns the events written, or -1. */
int mx_vm_trace_save(const char *path);

/* ---- the core heap ----------------------------------------------------------------- */

/* Core memory, with VM on or off: for what the DMAC follows, interrupt
   handlers, thread stacks. align: a power of two (16 at least). */
void *mx_core_alloc(u32 bytes, u32 align);
void mx_core_free(void *p);
/* How full the core heap is (to size mx_vm_config.core_heap): bytes it can
   reach in all (with VM on: what it was given), in use (headers included),
   and the largest allocation (16-byte aligned) that fits now. */
struct mx_core_stats {
  u32 total, used, largest_free;
};
void mx_core_stats(struct mx_core_stats *s);
/* DMA buffers: core, aligned to a cache line (a write back never touches a
   neighbour). */
MX_INLINE void *mx_dma_alloc(u32 bytes) { return mx_core_alloc((bytes + 63) & ~63u, 64); }

/* The program break (libmxc's sbrk): VM when VM is on, otherwise the core
   heap's other end. Returns the old break, or (void *)-1. */
void *mx_sbrk(s32 incr);

#endif
