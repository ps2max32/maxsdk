#include <mx/cpu.h>

/* R5900 data cache operations (EE Core User's Manual, CACHE instruction):
   0x1C DHWOIN = hit write back without invalidate, 0x1A DHIN = hit
   invalidate, each of one 64-byte line; 0x14 DXWBIN = index write back and
   invalidate, of the line of way vAddr[0] in set vAddr[11:6]. */

/* Only cached addresses: the operations are undefined on uncached, UCAB and
   scratchpad addresses. MX_PHYS_WIN, where pinned memory is, is cached too. */
static int cached(const void *p) {
  uintptr_t a = (uintptr_t)p;
  return a < 0x20000000u || (a >= MX_PHYS_WIN && a < MX_PHYS_WIN_LIMIT);
}

void mx_dcache_writeback(const void *p, u32 size) {
  if (!cached(p))
    return;
  /* as many lines as the cache has or more: all of the cache, by index
     (64 sets of 2 ways) */
  if (size >= MX_DCACHE_BYTES) {
    mx_dcache_flush_all();
    return;
  }
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)(MX_DCACHE_LINE - 1);
  uintptr_t end = (uintptr_t)p + size;
  for (; a < end; a += MX_DCACHE_LINE)
    __asm__ volatile("sync.l\n\tcache 0x1c, 0(%0)\n\tsync.l" : : "r"(a) : "memory");
}

void mx_dcache_invalidate(const void *p, u32 size) {
  if (!cached(p))
    return;
  uintptr_t a = (uintptr_t)p & ~(uintptr_t)(MX_DCACHE_LINE - 1);
  uintptr_t end = (uintptr_t)p + size;
  for (; a < end; a += MX_DCACHE_LINE)
    __asm__ volatile("sync.l\n\tcache 0x1a, 0(%0)\n\tsync.l" : : "r"(a) : "memory");
}

/* The index operations' addresses: 0x8_0000 (the kernel's, mapped in every
   address space), not kseg0: programs run in user mode, where a kseg0
   address does nothing (an index loop at 0x8000_0000 left the cache dirty:
   tests/ps2/vm7_swap.c's tables lost their headers). */
#define MX_CACHE_INDEX_BASE 0x00080000u

/* All of the data cache, by index (64 sets of 2 ways); each CACHE directly
   between two SYNCs (EE Core User's Manual 1.3: not after a branch, nor
   before one) */
void mx_dcache_flush_all(void) {
  for (uintptr_t a = MX_CACHE_INDEX_BASE; a < MX_CACHE_INDEX_BASE + MX_DCACHE_BYTES / 2;
       a += MX_DCACHE_LINE)
    __asm__ volatile("sync.l\n\tcache 0x14, 0(%0)\n\tsync.l\n\tcache 0x14, 1(%0)\n\tsync.l"
                     : : "r"(a) : "memory");
}

/* 0x07 IXIN = index invalidate of the instruction cache line of way
   vAddr[0] in set vAddr[12:6] (16 KiB, 128 sets of 2 ways), 0x0C BFH = BTAC
   flush (EE Core Instruction Set Manual). Each after a SYNC.P; the address's
   bit 31 must be 0 for the instruction cache's operations too (Restrictions
   manual, 13). */
#define MX_ICACHE_BYTES 16384
void mx_icache_invalidate_all(void) {
  for (uintptr_t a = MX_CACHE_INDEX_BASE; a < MX_CACHE_INDEX_BASE + MX_ICACHE_BYTES / 2;
       a += MX_DCACHE_LINE)
    __asm__ volatile("sync.p\n\tcache 0x07, 0(%0)\n\tsync.p\n\tcache 0x07, 1(%0)\n\tsync.p"
                     : : "r"(a) : "memory");
  __asm__ volatile("sync.p\n\tcache 0x0c, 0($0)\n\tsync.p" : : : "memory");
}
