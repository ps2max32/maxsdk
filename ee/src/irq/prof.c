/* PC-sampling profiler (<mx/prof.h>). */

#include <mx/mx.h>
#include <mx/prof.h>
#include <mx/timer.h>
#include <mx/vm.h>

#define NB 8192 /* buckets, a power of two */

static struct {
  u32 key; /* PC >> 4, + 1 (0: empty) */
  u32 n;
} b[NB];
static u32 total, lost;
static int timer = -1;

static void sample(void *arg) {
  (void)arg;
  u32 epc = mx_timer_epc();
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

/* with VM on: what the pager and the IOP's waits did between start and
   stop, for the report */
void mx__sif_waits(u32 out[4]);
static struct mx_vm_stats vs[2];
static u32 sw[2][4];
static void window(int k) {
  if (!mx_vm_active())
    return;
  mx_vm_stats(&vs[k]);
  mx__sif_waits(sw[k]);
}

void mx_prof_start(u32 hz) {
  for (u32 i = 0; i < NB; i++)
    b[i].key = b[i].n = 0;
  total = lost = 0;
  window(0);
  u32 us = 1000000u / (hz ? hz : 1000);
  if (timer >= 0)
    mx_timer_remove(timer);
  timer = mx_timer_add(us ? us : 1, us ? us : 1, sample, 0);
}

void mx_prof_stop(void) {
  if (timer >= 0)
    mx_timer_remove(timer);
  timer = -1;
  window(1);
}

void mx_prof_report(u32 top) {
  mx_printf("prof: total %u lost %u\n", total, lost);
  if (mx_vm_active()) {
    const struct mx_vm_stats *a = &vs[0], *z = &vs[1];
    /* two lines: long ones were lost on the PS2 (ps2link) */
    mx_printf("prof: meanwhile VM %u faults, %u swap ins (%u ms), %u outs (%u ms), %u by the "
              "writer, %u drops, %u from files (%u ms), %u zero fills\n",
              z->faults - a->faults,
              z->swap_ins - a->swap_ins, (z->us_swap_in - a->us_swap_in) / 1000,
              z->swap_outs + z->writer_outs - a->swap_outs - a->writer_outs,
              (z->us_swap_out - a->us_swap_out) / 1000, z->writer_outs - a->writer_outs,
              z->drops - a->drops, z->file_ins - a->file_ins,
              (z->us_file_in - a->us_file_in) / 1000, z->zero_fills - a->zero_fills);
    mx_printf("prof: meanwhile IOP waits %u in %u ms, the file server's %u in %u ms; %u TLB "
              "refills (%u of code; about %u ms)\n",
              sw[1][0] - sw[0][0], (sw[1][1] - sw[0][1]) / 1000, sw[1][2] - sw[0][2],
              (sw[1][3] - sw[0][3]) / 1000, z->refills - a->refills, z->refills_i - a->refills_i,
              (z->refills - a->refills) / (MX_EE_HZ / 1000 / MX_VM_REFILL_CYCLES));
  }
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
