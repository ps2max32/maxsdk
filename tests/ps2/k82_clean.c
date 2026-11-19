/* mx_vm_clean (kernel version 43, <mx/vm.h>): the program's written pages
   go to the swap file and stay resident. The program writes 4 MiB of its
   VM (a static array) and 1 MiB of a paged heap (the region), and checks:
   - mx_vm_clean writes at least those 160 pages, counted in swap_outs;
   - they are still resident and read right;
   - with dirty tracking (the PS2), a second call finds (almost) nothing,
     reading changes nothing, and three pages written again are written
     again (PCSX2 has none: every mapped page counts as written).
   Needs a swap file (MXOS's, or HOSTSWAP).
     k82_clean.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define BIG (4u << 20)
#define HEAP (1u << 20)
#define PAGES ((BIG + HEAP) / PAGE)
/* what the program writes besides (its data, the console's buffers) */
#define SLACK 8

static u32 big[BIG / 4]; /* VM: the program's .bss */

static u32 E(u32 i) { return i * 2654435761u ^ 0x3C3C3C3Cu; }

static u32 wrong(const u32 *m, u32 words, u32 salt) {
  u32 bad = 0;
  for (u32 i = 0; i < words; i += 256)
    bad += m[i] != (E(i) ^ salt);
  return bad;
}

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 43);
  u32 *heap = mx_heap_alloc(mx_heap_paged, HEAP, 64);
  CHECK(heap != 0);
  if (!heap)
    return t_done("k82");
  for (u32 i = 0; i < BIG / 4; i += 256)
    big[i] = E(i);
  for (u32 i = 0; i < HEAP / 4; i += 256)
    heap[i] = E(i) ^ 1;

  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  u32 t0 = mx_count();
  int n = mx_vm_clean();
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000u);
  mx_vm_stats(&s1);
  mx_printf("k82: mx_vm_clean wrote %d pages in %u ms (%u swap outs); dirty tracking %u\n", n, ms,
            s1.swap_outs - s0.swap_outs, s1.dirty_tracking);
  if (n < 0) {
    mx_printf("k82: no swap file: run it with one (HOSTSWAP)\n");
    CHECK(n >= 0);
    return t_done("k82");
  }
  CHECK((u32)n >= PAGES);
  CHECK_EQ(s1.swap_outs - s0.swap_outs, (u32)n);
  CHECK(mx_mem_resident(big, BIG));
  CHECK(mx_mem_resident(heap, HEAP));
  CHECK_EQ(wrong(big, BIG / 4, 0), 0);
  CHECK_EQ(wrong(heap, HEAP / 4, 1), 0);

  if (s1.dirty_tracking) {
    int again = mx_vm_clean(); /* only what was written since: the program's own bits */
    mx_printf("k82: again: %d pages\n", again);
    CHECK(again >= 0 && again <= SLACK);
    CHECK_EQ(wrong(big, BIG / 4, 0), 0);
    int read = mx_vm_clean();
    CHECK(read >= 0 && read <= SLACK);
    big[0] ^= 5, big[BIG / 8] ^= 5, heap[HEAP / 8] ^= 5;
    int three = mx_vm_clean();
    mx_printf("k82: three pages written: %d pages\n", three);
    CHECK(three >= 3 && three <= 3 + SLACK);
    big[0] ^= 5, big[BIG / 8] ^= 5, heap[HEAP / 8] ^= 5;
    CHECK_EQ(wrong(big, BIG / 4, 0), 0);
    CHECK_EQ(wrong(heap, HEAP / 4, 1), 0);
  }
  mx_heap_free(mx_heap_paged, heap);
  return t_done("k82");
}
