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

/* ---- DMA and the IOP --------------------------------------------------------- */

/* Pin [p, p + bytes) for the DMAC or the IOP: its pages are made resident
   and stay so, at the same frames, until unpinned (pins count). From a
   thread only (it may wait for the swap file). Returns 0, or -1 (not VM,
   or MX_VM_PIN_DMA and no run of frames that long). Core memory: 0,
   nothing to do. */
#define MX_VM_PIN_WRITE 1 /* the DMAC or the IOP writes the memory */
#define MX_VM_PIN_DMA 2   /* in one physical piece (mx_paddr_range works on it) */
int mx_vm_pin(const void *p, u32 bytes, u32 flags);
void mx_vm_unpin(const void *p, u32 bytes);
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
  u32 refills;    /* TLB refills (no entry) */
  u32 zero_fills; /* first touches */
  u32 faults;     /* page faults handled in the thread: new pages, the swap file */
  u32 swap_ins, swap_outs, drops; /* drops: clean pages evicted without I/O */
  u32 file_ins;                   /* pages of a program linked into VM read from its ELF */
  u32 retries;                    /* swap I/O repeated (the file server did not answer) */
  u32 dirty_tracking;             /* 1: TLB modified exceptions (the PS2), 0: every
                                     mapped page counts as written (PCSX2) */
};
void mx_vm_stats(struct mx_vm_stats *s);

/* ---- the core heap ----------------------------------------------------------------- */

/* Core memory, with VM on or off: for what the DMAC follows, interrupt
   handlers, thread stacks. align: a power of two (16 at least). */
void *mx_core_alloc(u32 bytes, u32 align);
void mx_core_free(void *p);
/* DMA buffers: core, aligned to a cache line (a write back never touches a
   neighbour). */
MX_INLINE void *mx_dma_alloc(u32 bytes) { return mx_core_alloc((bytes + 63) & ~63u, 64); }

/* The program break (libmxc's sbrk): VM when VM is on, otherwise the core
   heap's other end. Returns the old break, or (void *)-1. */
void *mx_sbrk(s32 incr);

#endif
