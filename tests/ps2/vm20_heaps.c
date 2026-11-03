/* VM step 20: heaps by kind (<mx/mem.h>) and the VM's TLB. A plain (not
   VM) program with mx_vm_config: 2 MiB of frames and a swap file.
     1 the TLB: the VM has 30 entries or more, no two entries match one
       address, the physical window reads what the BIOS's mapping reads
       (the program's own data)
     2 pinned: 40 small allocations share 64 KiB runs; each is in one
       physical piece (MX_UCAB writes, the cached view reads them back);
       freed, the frames go back to the pool
     3 pinned 512 KiB: one run of frames out of the pool while 3 MiB of
       pages are in use (pages move or go to the swap file), its data
       right, sent by DMA to the scratchpad and back
     4 an asset heap: 64 KiB written, mx_mem_lock_try locks it (resident)
       and mx_paddr_range gives one piece; after 3 MiB of other pages it is
       swapped out: lock_try says -1 and the pager brings it in, then it
       locks; freed, its pages are dropped (no swap out)
     5 the defaults: mx_heap_paged, mx_dma_alloc */

#include "t.h"
#include <mx/dma.h>
#include <mx/mem.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE

const struct mx_vm_config mx_vm_config = {.swap = "host:vm20swap.bin", .frames = 64 * PAGE};

u32 mx__vm_check(void);
static u32 probe_data[16] = {0x12345678u, 0x9ABCDEF0u};

/* touch n pages of VM (96 pages 8 MiB into it), so the frames fill and
   other pages go */
static void churn(u32 n) {
  u8 *filler = (u8 *)mx_vm_base() + (8u << 20);
  for (u32 i = 0; i < n; i++)
    filler[(i % 96) * PAGE + (i * 64) % PAGE] = (u8)i;
}

int main(void) {
  CHECK(mx_vm_active());
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("vm20: %u frames, %u TLB entries for the VM\n", s.frames, s.tlb_entries);

  /* 1 */
  CHECK(s.tlb_entries >= 30);
  CHECK_EQ(mx__vm_check(), 0);
  const volatile u32 *w = (const volatile u32 *)(uintptr_t)(MX_PHYS_WIN + mx_paddr_of(probe_data).v);
  mx_dcache_writeback(probe_data, sizeof probe_data);
  CHECK_EQ(w[0], 0x12345678u);
  CHECK_EQ(w[1], 0x9ABCDEF0u);

  /* 2 */
  u32 f0 = s.pinned_frames;
  u8 *small[40];
  for (int i = 0; i < 40; i++) {
    small[i] = mx_heap_alloc(mx_heap_pinned, 1000 + 37 * (u32)i, 64);
    CHECK(small[i] != 0);
    CHECK(!mx_is_vm(small[i]));
    CHECK(((uintptr_t)small[i] & 63) == 0);
    volatile u32 *u = mx_mem_ucab(small[i]);
    for (u32 k = 0; k < 250; k++)
      u[k] = 0xA5000000u | (u32)i << 12 | k;
  }
  mx_vm_stats(&s);
  mx_printf("vm20 2: 40 small allocations in %u frames\n", s.pinned_frames - f0);
  CHECK(s.pinned_frames - f0 <= 6);
  for (int i = 0; i < 40; i++) {
    mx_dcache_invalidate(small[i], 1024);
    const u32 *c = (const u32 *)small[i];
    CHECK_EQ(c[0], 0xA5000000u | (u32)i << 12);
    CHECK_EQ(c[249], 0xA5000000u | (u32)i << 12 | 249);
  }
  for (int i = 0; i < 40; i++)
    mx_heap_free(mx_heap_pinned, small[i]);
  mx_vm_stats(&s);
  CHECK(s.pinned_frames <= f0 + 1); /* the heap's table: a frame */

  /* 3 */
  churn(96);
  u32 *big = mx_heap_alloc(mx_heap_pinned, 512 * 1024, 64);
  CHECK(big != 0);
  mx_vm_stats(&s);
  mx_printf("vm20 3: 512 KiB pinned: %u frames pinned, %u free, %u swap outs\n", s.pinned_frames,
            s.free_frames, s.swap_outs);
  CHECK_EQ(mx_mem_run(big, 512 * 1024), 512 * 1024);
  for (u32 k = 0; k < 128 * 1024; k++)
    big[k] = k * 2654435761u;
  churn(200);
  mx_dcache_writeback(big, 512 * 1024);
  static u32 spr_back[1024] MX_ALIGNED(64);
  mx_dma_to_spr((void *)0x70000000, big + 64 * 1024, 1024 / 4);
  mx_dma_wait(MX_CH_TOSPR);
  mx_dma_from_spr(spr_back, (const void *)0x70000000, 1024 / 4);
  mx_dma_wait(MX_CH_FROMSPR);
  mx_dcache_invalidate(spr_back, sizeof spr_back);
  CHECK_EQ(spr_back[0], (64u * 1024) * 2654435761u);
  CHECK_EQ(spr_back[1023], (64u * 1024 + 1023) * 2654435761u);
  CHECK_EQ(big[128 * 1024 - 1], (128u * 1024 - 1) * 2654435761u);
  CHECK_EQ(mx__vm_check(), 0);
  mx_heap_free(mx_heap_pinned, big);

  /* 4 */
  struct mx_heap *asset = mx_heap_create("assets", MX_MEM_ASSET, 1u << 20);
  CHECK(asset != 0);
  u32 *a = mx_heap_alloc(asset, 64 * 1024, 64);
  CHECK(a != 0);
  CHECK(mx_is_vm(a));
  for (u32 k = 0; k < 16 * 1024; k++)
    a[k] = ~k;
  CHECK_EQ(mx_mem_lock_try(a, 64 * 1024), 0);
  CHECK_EQ(mx_mem_run(a, 64 * 1024), 64 * 1024);
  mx_vm_unlock(a, 64 * 1024);
  churn(400);
  int r = mx_mem_lock_try(a, 64 * 1024);
  mx_printf("vm20 4: after the churn lock_try %d, resident %d\n", r, mx_mem_resident(a, 64 * 1024));
  u32 t0 = mx_count();
  while (r < 0 && mx_count() - t0 < 5 * MX_EE_HZ) /* the pager runs above this thread */
    r = mx_mem_lock_try(a, 64 * 1024);
  CHECK_EQ(r, 0);
  CHECK_EQ(a[0], ~0u);
  CHECK_EQ(a[16 * 1024 - 1], ~(16u * 1024 - 1));
  mx_vm_unlock(a, 64 * 1024);
  u32 used, held;
  mx_heap_stats(asset, &used, &held);
  CHECK_EQ(used, 64 * 1024);
  mx_heap_free(asset, a);
  mx_heap_stats(asset, &used, &held);
  CHECK_EQ(used, 0);
  CHECK_EQ(held, 0);

  /* 5 */
  u32 *pg = mx_heap_alloc(mx_heap_paged, 100 * 1024, 16);
  CHECK(pg != 0 && mx_is_vm(pg));
  pg[0] = 1, pg[100 * 256 - 1] = 2;
  mx_heap_free(mx_heap_paged, pg);
  void *d = mx_dma_alloc(1000);
  CHECK(d != 0 && !mx_is_vm(d));
  mx_dma_free(d);
  CHECK_EQ(mx__vm_check(), 0);
  return t_done("vm20_heaps");
}
