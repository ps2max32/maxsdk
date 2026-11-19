/* What a TLB refill of a resident page costs under the kernel, in EE
   cycles: 96 resident pages (more than the TLB's entries for VM, so each
   read of the next refills) read in turn, a word each, 64 rounds; the same
   loop over one page (no refill) is taken off. For the VM window
   (the program's .bss) and for a heap of pages (the region). The kernel's tables are in two
   levels from version 42 (docs/vm.md, Tables): one more load a refill.
   Not a timing on PCSX2.
     k81_refill.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define NP 96
#define ROUNDS 64

static volatile u32 sink;
static u8 vmbuf[(NP + 1) * PAGE]; /* the program is linked into VM: its .bss is the VM window */

/* cycles and refills of ROUNDS rounds over np pages from base */
static u32 run(const u8 *base, u32 np, u32 *refills) {
  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  u32 t0 = mx_count();
  for (u32 r = 0; r < ROUNDS; r++)
    for (u32 i = 0; i < NP; i++)
      sink += *(const volatile u32 *)(base + (i % np) * PAGE + 64);
  u32 t = mx_count() - t0;
  mx_vm_stats(&s1);
  *refills = s1.refills - s0.refills;
  return t;
}

static void measure(const char *what, u8 *base) {
  for (u32 i = 0; i < NP; i++) /* resident, and written: no fault below */
    *(volatile u32 *)(base + i * PAGE + 64) = i;
  u32 r1, r2, best = ~0u, best_r = 0, one = ~0u;
  for (int k = 0; k < 5; k++) {
    u32 t = run(base, NP, &r1);
    if (t < best)
      best = t, best_r = r1;
    t = run(base, 1, &r2);
    if (t < one)
      one = t;
  }
  u32 per = best_r ? (best - one) / best_r : 0;
  mx_printf("k81: %s: %u cycles a refill (%u refills, %u cycles; %u without)\n", what, per, best_r,
            best, one);
  CHECK(best_r >= ROUNDS * NP / 2); /* nearly every read refilled */
}

int main(void) {
  u8 *a = vmbuf;
  CHECK(mx_is_vm(a));
  measure("the VM window", (u8 *)(((uintptr_t)a + PAGE - 1) & ~(uintptr_t)(PAGE - 1)));
  struct mx_heap *h = mx_heap_create("k81", MX_MEM_PAGED, (NP + 1) * PAGE);
  u8 *b = h ? mx_heap_alloc(h, NP * PAGE, PAGE) : 0;
  CHECK(b != 0);
  if (b)
    measure("a heap of pages", b);
  mx_printf("k81: kernel %u\n", mx__kernel ? mx__kernel->version : 0);
  return t_done("k81");
}
