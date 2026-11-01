#ifndef MX_CPU_H
#define MX_CPU_H

/* EE core: address spaces, register access, interrupts on/off, cache and
   COP0 counters. */

#include <mx/types.h>

/* ---- Address spaces ---------------------------------------------------- */

/* Main memory is seen three ways: cached (0x0...), uncached (0x2...) and
   uncached accelerated (UCAB, 0x3...). The scratchpad is at 0x7000_0000. DMA
   needs the physical address (docs/dma.md: an uncached alias crashed the
   PS2). */
#define MX_UNCACHED(p) ((__typeof__(&(p)[0]))(((uintptr_t)(p) & 0x0FFFFFFFu) | 0x20000000u))
#define MX_UCAB(p) ((__typeof__(&(p)[0]))(((uintptr_t)(p) & 0x0FFFFFFFu) | 0x30000000u))
#define MX_CACHED(p) ((__typeof__(&(p)[0]))((uintptr_t)(p) & 0x0FFFFFFFu))

MX_INLINE int mx_is_spr(const void *p) { return ((uintptr_t)p & 0xF0000000u) == 0x70000000u; }

/* Virtual memory (<mx/vm.h>): 0x0800_0000 - 0x0FFF_FFFF, pages that may be
   anywhere in physical memory or in swap. Not RAM, so a VM pointer taken
   for a physical address faults instead of reaching other memory. Its
   uncached alias (MX_UNCACHED) is not mapped. */
#define MX_VM_BASE 0x08000000u
#define MX_VM_LIMIT 0x10000000u
MX_INLINE int mx_is_vm(const void *p) { return ((uintptr_t)p >> 27) == 1; }

/* An address as the DMAC sees it (MADR, TADR, a tag's ADDR): physical, or
   an SPR offset with the SPR bit set. A type of its own, so a pointer is
   never taken for one or the other way round; only mx_paddr_of and
   mx_paddr_range make one. */
typedef struct {
  u32 v;
} mx_paddr;

void mx__paddr_fault(const void *p, u32 bytes) __attribute__((noreturn));
mx_paddr mx__vm_paddr(const void *p, u32 bytes);

/* Core memory (the program, the core heap, the stack) or the scratchpad:
   anything but VM, which ends the program with a report (a VM page is not
   at one physical address). */
MX_INLINE mx_paddr mx_paddr_of(const void *p) {
  uintptr_t a = (uintptr_t)p;
  if (__builtin_expect(mx_is_vm(p), 0))
    mx__paddr_fault(p, 0);
  if (mx_is_spr(p))
    return (mx_paddr){(u32)((a & 0x3FFFu) | 0x80000000u)};
  return (mx_paddr){(u32)(a & 0x0FFFFFFFu)};
}

/* [p, p + bytes): core memory, the scratchpad, or VM pinned in one
   physical piece (mx_vm_pin with MX_VM_PIN_DMA); other VM ends the program
   with a report. */
MX_INLINE mx_paddr mx_paddr_range(const void *p, u32 bytes) {
  if (__builtin_expect(mx_is_vm(p), 0))
    return mx__vm_paddr(p, bytes);
  return mx_paddr_of(p);
}

MX_INLINE mx_paddr mx_paddr_add(mx_paddr a, u32 bytes) { return (mx_paddr){a.v + bytes}; }

/* For a static, zero-initialised variable that the DMAC or the IOP writes
   (SIF receive buffers, RPC results), or that is written uncached while
   other code may have its line in the cache: cache lines of its own. Each
   variable of the section is aligned to a line and the section ends on
   one (ld/maxsdk.ld), so no other variable shares its lines, and a write
   back of another variable's dirty line never overwrites what was
   received. MX_ALIGNED(64) alone does not do that: the next variable may
   start in the last line. */
#define MX_DMA_LINES __attribute__((section(".bss.mx.lines"), aligned(64)))

/* ---- Register access --------------------------------------------------- */

#define MX_R32(a) (*(volatile u32 *)(uintptr_t)(a))
#define MX_R64(a) (*(volatile u64 *)(uintptr_t)(a))

/* 128-bit store, for FIFOs that take whole quadwords. */
MX_INLINE void mx_w128(u32 addr, u128 v) {
  u64 lo = mx_qw_lo(v), hi = mx_qw_hi(v);
  __asm__ volatile("pcpyld $8, %1, %0\n\tsq $8, 0(%2)"
                   :
                   : "r"(lo), "r"(hi), "r"(addr)
                   : "$8", "memory");
}

/* ---- Barriers and interrupts ------------------------------------------- */

MX_INLINE void mx_sync(void) { __asm__ volatile("sync.l" ::: "memory"); }
MX_INLINE void mx_sync_p(void) { __asm__ volatile("sync.p" ::: "memory"); }
MX_INLINE void mx_barrier(void) { __asm__ volatile("" ::: "memory"); }

MX_INLINE u32 mx_status(void) {
  u32 s;
  __asm__ volatile("mfc0 %0, $12" : "=r"(s));
  return s;
}

MX_INLINE void mx_ei(void) { __asm__ volatile("ei" ::: "memory"); }

/* Disable interrupts. The R5900 can miss a DI that coincides with an
   interrupt, so it is repeated until Status.EIE reads 0 (EE Core User's
   Manual, the DI note). Returns whether they were enabled. */
MX_INLINE int mx_di(void) {
  u32 st = mx_status();
  int was = (st >> 16) & 1;
  /* with IE (bit 0) off no interrupt is taken: DI is then done at once
     (the erratum needs an interrupt coming in), and repeating it until
     EIE reads 0 hung the PS2 */
  do {
    __asm__ volatile("di\n\tsync.p" ::: "memory");
    st = mx_status();
  } while ((st & (1u << 16)) && (st & 1));
  return was;
}

MX_INLINE void mx_irq_restore(int was) {
  if (was)
    mx_ei();
}

/* ---- COP0 counters ----------------------------------------------------- */

/* Count runs at the EE clock (294.912 MHz). */
MX_INLINE u32 mx_count(void) {
  u32 c;
  __asm__ volatile("mfc0 %0, $9" : "=r"(c));
  return c;
}

/* Performance counters (COP0 reg 25). The assembler has no MFPC/MTPC/MTPS,
   so they are encoded by hand. */
MX_INLINE void mx_pccr_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c800\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE void mx_pcr0_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c801\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE void mx_pcr1_write(u32 v) {
  __asm__ volatile("move $8, %0\n\t.word 0x4088c803\n\tsync.p" : : "r"(v) : "$8");
}
MX_INLINE u32 mx_pcr0(void) {
  u32 v;
  __asm__ volatile(".word 0x4008c801\n\tmove %0, $8" : "=r"(v) : : "$8");
  return v;
}
MX_INLINE u32 mx_pcr1(void) {
  u32 v;
  __asm__ volatile(".word 0x4008c803\n\tmove %0, $8" : "=r"(v) : : "$8");
  return v;
}

/* ---- Cache ------------------------------------------------------------- */

#define MX_DCACHE_LINE 64
#define MX_DCACHE_BYTES 8192

/* Write back (and keep) the data cache lines covering [p, p + size); from
   MX_DCACHE_BYTES on, all of the cache, written back and invalidated (128
   operations instead of one a line). */
void mx_dcache_writeback(const void *p, u32 size);
/* Invalidate the data cache lines covering [p, p + size), without writing
   back. Lines only partly inside the range lose their other bytes too. */
void mx_dcache_invalidate(const void *p, u32 size);
/* Write back and invalidate the whole data cache (BIOS FlushCache(0)). */
void mx_dcache_flush_all(void);

#endif
