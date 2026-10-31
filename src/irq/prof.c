/* PC-sampling profiler (<mx/prof.h>). */

#include <mx/mx.h>
#include <mx/prof.h>
#include <mx/timer.h>

#define NB 8192 /* buckets, a power of two */

static struct {
  u32 key; /* PC >> 4, + 1 (0: empty) */
  u32 n;
} b[NB];
static u32 total, lost;

static void sample(void *arg) {
  (void)arg;
  extern volatile u32 mx__irq_epc;
  u32 epc = mx__irq_epc;
  u32 key = (epc >> 4) + 1;
  u32 h = (key * 2654435761u) >> 19;
  total++;
  for (u32 i = 0; i < 16; i++) {
    u32 at = (h + i) & (NB - 1);
    if (b[at].key == key) {
      b[at].n++;
      return;
    }
    if (!b[at].key) {
      b[at].key = key;
      b[at].n = 1;
      return;
    }
  }
  lost++;
}

void mx_prof_start(u32 hz) {
  for (u32 i = 0; i < NB; i++)
    b[i].key = b[i].n = 0;
  total = lost = 0;
  u32 period = (MX_BUS_HZ / 256) / (hz ? hz : 1000);
  mx_timer_periodic(1, period ? period : 1, sample, 0);
}

void mx_prof_stop(void) { mx_timer_periodic(1, 0, 0, 0); }

void mx_prof_report(u32 top) {
  mx_printf("prof: total %u lost %u\n", total, lost);
  /* the largest first, by taking the largest left each time */
  for (u32 k = 0; k < top; k++) {
    u32 best = 0, at = 0;
    for (u32 i = 0; i < NB; i++)
      if (b[i].key && b[i].n > best)
        best = b[i].n, at = i;
    if (!best)
      break;
    mx_printf("prof: %08x %u\n", (b[at].key - 1) << 4, best);
    b[at].n = 0;
  }
}
