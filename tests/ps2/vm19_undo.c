/* VM step 19: an mx_vm_init that fails gives back what it took: the core
   heap (its tables) and the break (the frames) are as before, and it can be
   tried again with the same result. A plain (not VM) program.
     1 too little memory for frames (core_heap: all of it)
     2 no core memory for the trace
     3 the defaults, twice: VM comes on (PS2, PCSX2's EE interpreter) or does
       not (PCSX2's recompiler raises no TLB misses); off, nothing is kept */

#include "t.h"
#include <mx/vm.h>

static struct mx_core_stats before;
static void *brk0;

static void same(const char *what) {
  struct mx_core_stats s;
  mx_core_stats(&s);
  void *brk = mx_sbrk(0);
  if (s.used != before.used || s.largest_free != before.largest_free || brk != brk0)
    mx_printf("vm19 %s: core used %u -> %u, largest free %u -> %u, break %p -> %p\n", what,
              before.used, s.used, before.largest_free, s.largest_free, brk0, brk);
  CHECK_EQ(s.used, before.used);
  CHECK_EQ(s.largest_free, before.largest_free);
  CHECK_EQ((uintptr_t)brk, (uintptr_t)brk0);
}

int main(void) {
  mx_core_stats(&before);
  brk0 = mx_sbrk(0);
  mx_printf("vm19: core used %u, largest free %u, break %p\n", before.used, before.largest_free,
            brk0);

  const struct mx_vm_config all = {.core_heap = 32u << 20};
  CHECK_EQ(mx_vm_init(&all), -1);
  CHECK(!mx_vm_active());
  same("1 frames");

  const struct mx_vm_config trace = {.trace = 32u << 20};
  CHECK_EQ(mx_vm_init(&trace), -1);
  CHECK(!mx_vm_active());
  same("2 trace");

  const struct mx_vm_config def = {0};
  int r = mx_vm_init(&def);
  if (r == 0) {
    mx_printf("vm19 3: VM is on (TLB misses raised): nothing to give back\n");
    CHECK(mx_vm_active());
  } else {
    CHECK(!mx_vm_active());
    same("3 off");
    CHECK_EQ(mx_vm_init(&def), -1);
    same("3 off again");
  }
  return t_done("vm19_undo");
}
