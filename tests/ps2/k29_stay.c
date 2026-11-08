/* Step 2c (docs/mx-os-plan.md): a libmxk program without VM with its core
   at a run of its own (MX_CORE_HEAP) starts another and goes on when that
   ends: mx_loader_launch returns its exit status, and its memory, its core
   offset and its ASID are as they were. Started by mxlink:
     k29_stay.elf host:vm26_launch.elf child     (status 0 expected)
   The other's run goes below this one's. */
#include "t.h"
#include <mx/loader.h>
#include <mx/string.h>
#include <mx/vm.h>

MX_CORE_HEAP(512 << 10);

static u32 pattern[4096];

static u32 asid(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v & 0xFFu;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k29: usage: k29_stay.elf <program> [args]\n");
    return t_done("k29");
  }
  for (u32 i = 0; i < 4096; i++)
    pattern[i] = i * 2654435761u;
  u32 *heap = mx_sbrk(64 << 10);
  CHECK(heap != (u32 *)-1);
  for (u32 i = 0; heap != (u32 *)-1 && i < (16 << 10); i++)
    heap[i] = ~i;
  u32 off = mx__core_off, a = asid();
  mx_printf("k29: core offset %08x, ASID %u, heap %p; starting %s\n", off, a, (void *)heap, argv[1]);
  CHECK(off != 0);

  static char args[256];
  u32 n = 0;
  for (int i = 1; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  int st = mx_loader_launch(argc - 1, args, n, 0);
  mx_printf("k29: back, status %d\n", st);
  CHECK_EQ(st, 0);
  CHECK_EQ(mx__core_off, off);
  CHECK_EQ(asid(), a);
  u32 bad = 0;
  for (u32 i = 0; i < 4096; i++)
    bad += pattern[i] != i * 2654435761u;
  for (u32 i = 0; heap != (u32 *)-1 && i < (16 << 10); i++)
    bad += heap[i] != ~i;
  mx_printf("k29: %u words changed\n", bad);
  CHECK_EQ(bad, 0);
  return t_done("k29");
}
