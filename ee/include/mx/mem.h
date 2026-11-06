#ifndef MX_MEM_H
#define MX_MEM_H

/* Memory by kind: heaps (docs/vm.md).

   With VM on (<mx/vm.h>) all memory besides the program, its stack and
   the VM's tables is one pool of 32 KiB frames, shared by
   - pinned memory (MX_MEM_PINNED, MX_MEM_STREAM): runs of frames taken out
     of the pool while in use (the pages in their way move or go to the swap
     file), at one physical address, reached through MX_PHYS_WIN (one TLB
     entry for all of it): what the DMAC reads or follows (tags, chains,
     rings, vertex and texture data), what interrupt handlers touch, thread
     stacks. mx_paddr_of, MX_UNCACHED and MX_UCAB work on it as on core
     memory, and it costs no VM TLB entries;
   - pages of VM (MX_MEM_PAGED, MX_MEM_ASSET): pageable, in the heap window
     (MX_VM_HEAP_BASE, apart from malloc's VM), each heap its own range.
     ASSET memory is read by DMA while locked (mx_mem_lock_try,
     mx_vm_lock + mx_frame_unlock).
   With VM off every kind is physical memory (the heap above the break),
   resident: the same calls work.

   A heap keeps its bookkeeping apart from its memory, so allocating and
   freeing never touch its pages. Pinned heaps grow by runs of frames (64
   KiB, or a run of its own for a larger allocation: every allocation is in
   one physical piece) and give a run back when nothing in it is used.
   Freed whole pages of a heap of pages are dropped (no swap write).
   Thread code with interrupts on (a pinned allocation may move pages). */

#include <mx/cpu.h>
#include <mx/types.h>

enum mx_mem_kind {
  MX_MEM_PAGED,  /* pageable, cached: data only the EE reads */
  MX_MEM_PINNED, /* resident at fixed frames: DMA data, tags and chains, interrupt data */
  MX_MEM_STREAM, /* pinned too: written once (often through MX_UCAB), read by DMA */
  MX_MEM_ASSET,  /* pageable; locked while DMA reads it (texture levels, model streams) */
};

struct mx_heap;
/* A heap. max_bytes: for MX_MEM_PAGED and MX_MEM_ASSET the VM it may use
   (taken from the heap window: mx_vm_config.heap_size); for the pinned
   kinds a limit (0: none). name: for reports (kept, not copied). Returns 0
   when out of heaps (16) or heap window. */
struct mx_heap *mx_heap_create(const char *name, int kind, u32 max_bytes);
/* bytes, aligned to align (a power of two, 16 at least; 64 for what the
   DMAC reads: a cache line of its own). Returns 0 when out of memory. */
void *mx_heap_alloc(struct mx_heap *h, u32 bytes, u32 align);
void mx_heap_free(struct mx_heap *h, void *p);
/* everything in it freed: its runs back to the pool, its pages dropped.
   Nothing may read it any more (frames in flight: the caller waits). */
void mx_heap_reset(struct mx_heap *h);
/* bytes in use (allocations), and held (its runs, or its pages resident) */
void mx_heap_stats(const struct mx_heap *h, u32 *used, u32 *held);
/* the defaults: pinned, stream, and pageable (16 MiB of the heap window,
   taken at its first use) */
extern struct mx_heap *const mx_heap_pinned, *const mx_heap_stream, *const mx_heap_paged;

/* DMA buffers: pinned, aligned to a cache line */
MX_INLINE void *mx_dma_alloc(u32 bytes) { return mx_heap_alloc(mx_heap_pinned, bytes, 64); }
MX_INLINE void mx_dma_free(void *p) { mx_heap_free(mx_heap_pinned, p); }

/* ---- addresses -------------------------------------------------------------- */

/* the DMAC's address of p: pinned or core memory, or locked VM */
MX_INLINE mx_paddr mx_mem_paddr(const void *p) { return mx_paddr_of(p); }
/* bytes physically contiguous from p, of at most bytes (pinned: all of
   them; locked VM: up to the end of its piece) */
u32 mx_mem_run(const void *p, u32 bytes);
/* its uncached and UCAB views (pinned or core memory; 0 for VM) */
MX_INLINE void *mx_mem_ucab(const void *p) { return mx_is_vm(p) ? 0 : (void *)MX_UCAB((const u8 *)p); }
MX_INLINE void *mx_mem_uncached(const void *p) {
  return mx_is_vm(p) ? 0 : (void *)MX_UNCACHED((const u8 *)p);
}

/* ---- assets --------------------------------------------------------------- */

/* Lock [p, p + bytes) for a transfer without waiting for the swap file:
   0 when it was resident and is now locked (mx_vm_unlock, or
   mx_frame_unlock once the frame that reads it is done); -1 when it is not
   resident: the pager thread brings it in, ask again later. Not VM: 0,
   nothing to unlock. */
int mx_mem_lock_try(const void *p, u32 bytes);
/* bring [p, p + bytes) in ahead (the pager thread), without waiting */
void mx_mem_prefetch(const void *p, u32 bytes);
/* [p, p + bytes) is not needed for now: its resident pages go first when
   frames are needed (dropped if clean). Not VM: nothing. */
void mx_mem_cold(const void *p, u32 bytes);
/* resident (touching or locking it waits for no file) */
int mx_mem_resident(const void *p, u32 bytes);

/* ---- pressure ------------------------------------------------------------- */

/* For policies (texture caches, games): the pool and its traffic, the
   rates over the time since the call before (at least 250 ms apart: a
   call sooner gives the same rates). VM off: frames 0. */
struct mx_mem_pressure {
  u32 frames, free_frames, pinned_frames;
  u32 swap_ins_s, swap_outs_s, refills_s;
};
void mx_mem_pressure(struct mx_mem_pressure *p);

#endif
