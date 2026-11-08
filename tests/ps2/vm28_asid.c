/* Step 2b (docs/mx-os-plan.md): a libmxk program runs under an ASID of its
   own (EntryHi), VM works under it. Started twice in a row, the second gets
   another ASID. On a kernel of version 9 its core is at a physical run of its
   own: mx_paddr_of adds mx__core_off, the uncached view and a DMA through the
   scratchpad reach the same memory. (A program cannot read the TLB itself:
   tlbr loads EntryHi, the CPU's ASID, and its own code is under its ASID.)
   Linked into VM, started by mxlink. */
#include "t.h"
#include <mx/dma.h>
#include <mx/vm.h>
#include <mx/mem.h>

static u32 entry_hi(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v;
}

int main(void) {
  u32 asid = entry_hi() & 0xFFu;
  mx_printf("vm28: ASID %u\n", asid);
  CHECK(asid != 0);

  /* VM pages touched: their entries in the TLB */
  u32 n = 64 * 1024;
  volatile u32 *p = mx_sbrk(n * 4);
  mx_printf("vm28: sbrk %p\n", (void *)p);
  CHECK(p != 0);
  CHECK(mx_is_vm((const void *)p));
  p[0] = 1;
  mx_printf("vm28: first store\n");
  for (u32 i = 0; i < n; i += 1024)
    p[i] = i ^ 0x5A5A5A5Au;
  mx_printf("vm28: stores\n");
  u32 bad = 0;
  for (u32 i = 0; i < n; i += 1024)
    bad += p[i] != (i ^ 0x5A5A5A5Au);
  CHECK_EQ(bad, 0);

  /* the core */
  static u32 probe[4] MX_DMA_LINES;
  mx_printf("vm28: core offset %08x\n", mx__core_off);
  CHECK_EQ(mx_paddr_of(probe).v, (u32)(uintptr_t)probe + mx__core_off);
  /* the DMAC sees physical addresses: to the scratchpad and back */
  static u32 src[64] MX_DMA_LINES, dst[64] MX_DMA_LINES;
  for (u32 i = 0; i < 64; i++)
    src[i] = 0xC0DE0000u + i, dst[i] = 0;
  mx_dcache_writeback(src, sizeof src);
  mx_dcache_writeback(dst, sizeof dst);
  mx_dcache_invalidate(dst, sizeof dst);
  void *spr = (void *)(uintptr_t)0x70002000u;
  mx_dma_to_spr(spr, src, sizeof src / 16);
  mx_dma_wait(MX_CH_TOSPR);
  mx_dma_from_spr(dst, spr, sizeof dst / 16);
  mx_dma_wait(MX_CH_FROMSPR);
  mx_dcache_invalidate(dst, sizeof dst);
  u32 dbad = 0;
  for (u32 i = 0; i < 64; i++)
    dbad += dst[i] != 0xC0DE0000u + i;
  mx_printf("vm28: DMA through the scratchpad: %u words wrong\n", dbad);
  CHECK_EQ(dbad, 0);
  /* through the uncached view: the same memory */
  probe[0] = 0x12345678u;
  mx_dcache_writeback(probe, 16);
  CHECK_EQ(MX_UNCACHED(probe)[0], 0x12345678u);

  /* the ASID stayed through all of it */
  CHECK_EQ(entry_hi() & 0xFFu, asid);
  return t_done("vm28");
}
