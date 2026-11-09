/* Timers as the kernel's service (kernel version 19; <mx/timer.h>). This
   program (libmxk without VM) has a software timer every 10 ms and claims
   an EE timer of its own; it starts vm35_timers in front (mx_proc_spawn),
   which has timers of its own and finds no EE timer free. Its timer goes on
   while the other runs and after its end (the end stops the EE timers, the
   kernel sets its own again). Started by mxlink:
     k42_timers.elf host:vm35_timers.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>

MX_CORE_HEAP(256 << 10);

static volatile u32 ticks MX_CORE_DATA;
MX_CORE_FN static void tick(void *arg) {
  (void)arg;
  ticks++;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k42: usage: k42_timers.elf <vm35_timers.elf>\n");
    return t_done("k42");
  }
  int id = mx_timer_add(10000, 10000, tick, 0);
  CHECK(id > 0);
  int t = mx_timer_claim(MX_TIMER_HBLANK);
  mx_printf("k42: timer %d every 10 ms; EE timer %d claimed\n", id, t);
  CHECK(t >= 0);
  CHECK_EQ(mx_timer_claim(MX_TIMER_BUS), -1); /* the other is the kernel's */

  static char args[256];
  u32 n = (u32)strlen(argv[1]) + 1;
  memcpy(args, argv[1], n);
  u32 t0 = ticks;
  int child = mx_proc_spawn(1, args, n, 0);
  CHECK(child > 0);
  int st = -1;
  CHECK_EQ(mx_proc_wait(child, &st, 0), 1);
  u32 during = ticks - t0;
  u32 t1 = ticks;
  mx_sleep_us(200000);
  u32 after = ticks - t1;
  mx_printf("k42: the other ended with %d; ticks meanwhile %u, in 200 ms after it %u\n", st,
            during, after);
  CHECK_EQ(st, 0);
  CHECK(during > 10);
  CHECK(after >= 15 && after <= 25);
  mx_timer_remove(id);
  u32 t2 = ticks;
  mx_sleep_us(50000);
  CHECK_EQ(ticks, t2); /* removed */
  mx_timer_release(t);
  CHECK_EQ(mx_timer_claim(MX_TIMER_HBLANK), t); /* given back, free again */
  mx_timer_release(t);
  return t_done("k42");
}
