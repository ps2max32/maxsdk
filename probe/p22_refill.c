/* Probe: what a TLB refill of a resident VM page costs, in EE cycles, with
   tlb.S's fast refill and without it (the full save and C, as before
   2026-11-01): 64 resident pages read in turn, one word each, 16 rounds
   (more pages than the 17 TLB entries, so each read refills). Also checks
   the tables after each (mx__vm_check) and that the data stayed right.
   On PCSX2 run it with PCSX2_EE=interp (and the numbers are not timings). */

#include <mx/mx.h>
#include <mx/vm.h>

#include "../src/vm/vm_int.h"

#define PAGE MX_VM_PAGE
#define NP 64

u32 mx__vm_check(void);
static volatile u32 sink;

static u32 *vmw(u32 pg) { return (u32 *)((u8 *)mx_vm_base() + pg * PAGE); }

static u32 rounds(u32 *refills, u32 *bad) {
  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  u32 t0 = mx_count();
  for (int r = 0; r < 16; r++)
    for (u32 pg = 0; pg < NP; pg++) {
      u32 v = *vmw(pg);
      *bad += v != pg * 7u;
      sink += v;
    }
  u32 t = mx_count() - t0;
  mx_vm_stats(&s1);
  *refills = s1.refills - s0.refills;
  return t;
}

int main(void) {
  static const struct mx_vm_config cfg = {.frames = 4u << 20};
  if (mx_vm_init(&cfg) < 0) {
    MX_TEST_END(1);
    return 1;
  }
  for (u32 pg = 0; pg < NP; pg++)
    *vmw(pg) = pg * 7u;
  u32 bad = 0, n, c, fail = 0;
  /* the loop alone: one page, always in the TLB */
  u32 t0 = mx_count();
  for (u32 i = 0; i < 16 * NP; i++)
    sink += *vmw(0);
  u32 loop = mx_count() - t0;

  for (int k = 0; k < 2; k++) {
    u32 keep = mx__vm_fast.npages;
    if (k == 1)
      mx__vm_fast.npages = 0; /* the full path */
    rounds(&n, &bad);         /* warm */
    u32 t = rounds(&n, &bad);
    mx__vm_fast.npages = keep;
    c = mx__vm_check();
    fail |= c != 0 || n < 16 * NP / 2;
    mx_printf("p22_refill: %s: %u cycles a refill (%u refills, %u reads), check %08x\n",
              k ? "full path (C)" : "fast path", n ? (t - loop) / n : 0, n, 16 * NP, c);
  }
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("p22_refill: %u refills, %u of code; %u reads wrong\n", s.refills, s.refills_i, bad);
  fail |= bad != 0;
  MX_TEST_END(fail ? 1 : 0);
  return fail;
}
