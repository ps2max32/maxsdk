/* Where a program's TLB refills fall (kernel version 35, <mx/vm.h>'s
   mx_vm_refill_hist), and the fast refill for all three windows. The
   program touches a page of its VM (a static array) and a page of a paged
   heap (the region) once every 20 ms, 30 times: the VM's entries leave
   the TLB at each vblank, so each touch after one is a refill. The
   histogram must then name both pages, each with about as many refills as
   vblanks passed, and after the call start them at 0 again.
     k67_hist.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/thread.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

static volatile u32 in_vm[4 * MX_VM_PAGE / 4]; /* VM: the program's .bss */
static struct mx_vm_hot hot[256];

static u32 count_of(int n, u32 va) {
  for (int i = 0; i < n; i++)
    if (hot[i].va == (va & ~(MX_VM_PAGE - 1)))
      return hot[i].count;
  return 0;
}

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 35);
  volatile u32 *heap = mx_heap_alloc(mx_heap_paged, 2 * MX_VM_PAGE, 64);
  CHECK(heap != 0);
  volatile u32 *a = &in_vm[2 * MX_VM_PAGE / 4], *b = heap + MX_VM_PAGE / 4;
  mx_printf("k67: a VM page at %p, a page of the region at %p\n", (void *)a, (void *)b);
  CHECK((u32)(uintptr_t)a - MX_VM_BASE < MX_VM_LIMIT - MX_VM_BASE);
  CHECK((u32)(uintptr_t)b >= MX_VM_REGION_BASE);
  CHECK_EQ(mx_vm_refill_hist(hot, 256), 0); /* started */
  u32 sum = 0;
  for (u32 n = 0; n < 30; n++) {
    *a = n, *b = n * 3;
    sum += *a + *b;
    mx_sleep_us(20000);
  }
  CHECK_EQ(sum, 29 * 30 / 2 * 4);
  int n = mx_vm_refill_hist(hot, 256);
  u32 ca = count_of(n, (u32)(uintptr_t)a), cb = count_of(n, (u32)(uintptr_t)b);
  mx_printf("k67: %d pages had refills; the VM page %u, the region's page %u\n", n, ca, cb);
  CHECK(n > 0 && n < 256);
  CHECK(ca >= 10 && ca <= 40);
  CHECK(cb >= 10 && cb <= 40);
  /* started again at 0: nothing touched since but this code and stack */
  n = mx_vm_refill_hist(hot, 256);
  CHECK(count_of(n, (u32)(uintptr_t)a) <= 1);
  CHECK(count_of(n, (u32)(uintptr_t)b) <= 1);
  return t_done("k67");
}
