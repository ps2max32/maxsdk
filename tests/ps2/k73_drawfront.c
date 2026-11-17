/* The drawing hardware's interrupts go to the process in front only (mx OS
   plan step 6, part 4): this program counts the vblank's start with a
   handler of its own, then starts itself as a child, which is in front
   for a second while this one runs beside it, and counts again once it is
   in front again. Beside, its handler is not called (before, a shell
   beside a game got the game's GS, VIF and DMA interrupts); in front it
   is, about 50 or 60 times a second.
     k73_drawfront.elf     (status 0 expected) */
#include "t.h"
#include <mx/irq.h>
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

static volatile u32 vblanks MX_IRQ_BSS;
static MX_IRQ_FN void count(void *arg) {
  (void)arg;
  vblanks++;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    mx_sleep_us(1000000);
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 38);
  mx_irq_intc(MX_INTC_VBON, count, 0);
  mx_sleep_us(500000);
  u32 front0 = vblanks;
  CHECK(front0 >= 20);

  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0), st = -1;
  CHECK(id > 0);
  u32 b0 = vblanks; /* from here the child is in front (its start took a few) */
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 5);
  u32 beside = vblanks - b0;

  u32 f0 = vblanks;
  mx_sleep_us(500000);
  u32 front = vblanks - f0;
  mx_irq_intc(MX_INTC_VBON, 0, 0);
  mx_printf("k73: vblanks in front %u in 0.5 s; beside a child in front for 1 s %u; in front again %u in 0.5 s\n",
            front0, beside, front);
  CHECK(beside <= 3); /* the switch back's own */
  CHECK(front >= 20);
  return t_done("k73");
}
