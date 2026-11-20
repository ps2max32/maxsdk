/* What a timer callback costs a program under the kernel: a loop of plain
   work, timed without a timer and with mx_timer_add every 500 and 100 us
   (a callback that only counts); the time it took longer, divided by the
   callbacks, is the cycles one costs the program (the kernel's way in and
   out, and what the program's code and data lose of the caches and the TLB).
   It also touches pages of its VM in the loop, so the TLB's refills after
   each callback count too. Prints the figures; status 0.
     k86_irqcost.elf */
#include "t.h"
#include <mx/kernel.h>
#include <mx/timer.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
static volatile u32 calls MX_IRQ_DATA;
MX_IRQ_FN static void tick(void *arg) {
  (void)arg;
  calls++;
}

static u32 data[8 * PAGE / 4]; /* VM: 8 pages the loop touches */

static inline u32 now(void) {
  __asm__ volatile("" ::: "memory");
  u32 c = mx_count();
  __asm__ volatile("" ::: "memory");
  return c;
}

/* plain work with a page of VM every 64 rounds: its cycles */
static u32 work(void) {
  u32 t0 = now(), acc = 0;
  for (u32 i = 0; i < 400000; i++) {
    acc = acc * 1664525u + 1013904223u;
    if (!(i & 63))
      acc += data[(i >> 6) % 8 * (PAGE / 4)]++;
  }
  __asm__ volatile("" : : "r"(acc));
  return now() - t0;
}

int main(void) {
  CHECK(mx__kernel != 0);
  work(); /* the pages in, the caches warm */
  u32 base = 0xFFFFFFFFu;
  for (int i = 0; i < 3; i++) {
    u32 t = work();
    if (t < base)
      base = t;
  }
  mx_printf("k86: the loop alone %u cycles (%u us)\n", base, base / (MX_EE_HZ / 1000000u));
  static const u32 periods[2] = {500, 100};
  for (int k = 0; k < 2; k++) {
    calls = 0;
    int id = mx_timer_add(periods[k], periods[k], tick, 0);
    CHECK(id >= 0);
    u32 c0 = calls, t = work(), n = calls - c0;
    mx_timer_remove(id);
    u32 extra = t > base ? t - base : 0;
    mx_printf("k86: a timer every %u us: %u cycles, %u callbacks, %u cycles each\n", periods[k], t, n,
              n ? extra / n : 0);
  }
  return t_done("k86");
}
