/* Single frames from the bottom, runs from the top (mx OS plan step 6,
   part 3): pages a process touches take free frames one at a time, and a
   run of frames for pinned memory or a core is taken from the top of the
   pool; a page in its way is moved out (the kernel counts them: "moved" in
   its "VM pool" line at each program's start). This program starts itself
   twice:
   - "fill": 850 pages of VM written, then the end (its frames free again);
   - "hold": 700 pages written, then 4 MiB of pinned memory (a run) while
     they are still its.
   With single frames taken round the pool, the second child's pages go
   where the first one's ended, near the top, and the run has to move them;
   from the bottom, the run finds the top free. The parent prints how the
   children ended; the moves are in the kernel's line of the next program.
     k71_framerun.elf     (status 0 expected) */
#include "t.h"
#include <mx/kernel.h>
#include <mx/mem.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE

static int touch(u32 pages) {
  struct mx_heap *h = mx_heap_create("k71", MX_MEM_PAGED, pages * PAGE);
  volatile u8 *p = h ? mx_heap_alloc(h, pages * PAGE, PAGE) : 0;
  if (!p)
    return -1;
  for (u32 i = 0; i < pages; i++)
    p[i * PAGE] = (u8)i;
  return 0;
}

static int child(const char *self, const char *what) {
  static char args[256];
  u32 n = (u32)strlen(self) + 1, w = (u32)strlen(what) + 1;
  memcpy(args, self, n);
  memcpy(args + n, what, w);
  int id = mx_proc_spawn(2, args, n + w, 0), st = -1;
  if (id <= 0 || mx_proc_wait(id, &st, 0) != 1)
    return -1;
  return st;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "fill"))
    return touch(850) < 0 ? 6 : 5;
  if (argc > 1 && !strcmp(argv[1], "hold")) {
    if (touch(700) < 0)
      return 6;
    volatile u8 *p = mx_dma_alloc(4u << 20);
    if (!p)
      return 7;
    for (u32 i = 0; i < (4u << 20); i += PAGE)
      p[i] = (u8)i;
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 38);
  CHECK_EQ(child(argv[0], "fill"), 5);
  CHECK_EQ(child(argv[0], "hold"), 5);
  return t_done("k71");
}
