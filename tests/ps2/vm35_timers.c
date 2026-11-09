/* Started by k42_timers (kernel version 19): software timers of its own
   (every 5 ms, once after 50 ms), mx_timer_remove, and no EE timer free (the
   other holds one, the kernel the other). Its exit status: what failed. */
#include "t.h"
#include <mx/thread.h>
#include <mx/timer.h>

static volatile u32 every MX_CORE_DATA, once MX_CORE_DATA;
MX_CORE_FN static void on_every(void *arg) {
  (void)arg;
  every++;
}
MX_CORE_FN static void on_once(void *arg) {
  (void)arg;
  once++;
}

int main(void) {
  int a = mx_timer_add(5000, 5000, on_every, 0);
  int b = mx_timer_add(50000, 0, on_once, 0);
  CHECK(a > 0 && b > 0);
  CHECK_EQ(mx_timer_claim(MX_TIMER_HBLANK), -1);
  mx_sleep_us(300000);
  u32 e = every;
  mx_printf("vm35: in 300 ms: every 5 ms %u times, once %u\n", e, once);
  CHECK(e >= 50 && e <= 65);
  CHECK_EQ(once, 1);
  mx_timer_remove(a);
  mx_timer_remove(b); /* gone already: nothing */
  u32 e2 = every;
  mx_sleep_us(50000);
  CHECK_EQ(every, e2);
  return t_done("vm35");
}
