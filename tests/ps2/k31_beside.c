/* Step 2e-2 (docs/mx-os-apps-plan.md): two processes side by side. A libmxk
   program without VM with its core at a run of its own (MX_CORE_HEAP)
   starts another with mx_loader_launch_beside, and its worker thread goes on
   while the other runs, whenever the other waits: its core memory (at the
   same virtual addresses as the other's), its ASID, VU0's registers and its
   vblank handler stay its own. Started by mxlink:
     k31_beside.elf host:vm32_side.elf     (status 0 expected) */
#include "t.h"
#include <mx/irq.h>
#include <mx/loader.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/vm.h>

MX_CORE_HEAP(512 << 10);

#define N 4096
static u32 pattern[N];
static volatile u32 vblanks MX_CORE_DATA; /* the vblank handler's: core */
static volatile u32 rounds, bad_mem, bad_vu, bad_asid, stop, done;
static u32 my_asid;
static u8 stack[16 << 10] MX_ALIGNED(16) MX_CORE_DATA; /* a stack: core */

static u32 asid(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v & 0xFFu;
}

MX_CORE_FN static void vblank(void *arg) {
  (void)arg;
  vblanks++;
}

/* vf1-vf8: base + n in each word */
#define VF_SET(n)                                                                          \
  q[0] = q[1] = q[2] = q[3] = base + n;                                                    \
  __asm__ volatile("lq $8, 0(%0)\n\tqmtc2 $8, $vf" #n : : "r"(q) : "$8", "memory");
#define VF_GET(n)                                                                          \
  __asm__ volatile("qmfc2 $8, $vf" #n "\n\tsq $8, 0(%0)" : : "r"(q) : "$8", "memory");     \
  bad += q[0] != base + n || q[1] != base + n || q[2] != base + n || q[3] != base + n;
static void vu_set(u32 base) {
  u32 q[4] MX_ALIGNED(16);
  VF_SET(1) VF_SET(2) VF_SET(3) VF_SET(4) VF_SET(5) VF_SET(6) VF_SET(7) VF_SET(8)
}
static u32 vu_check(u32 base) {
  u32 q[4] MX_ALIGNED(16), bad = 0;
  VF_GET(1) VF_GET(2) VF_GET(3) VF_GET(4) VF_GET(5) VF_GET(6) VF_GET(7) VF_GET(8)
  return bad;
}

static void worker(void *arg) {
  (void)arg;
  vu_set(0x1000);
  while (!stop) {
    u32 b = 0;
    for (u32 i = 0; i < N; i++)
      b += pattern[i] != i * 2654435761u;
    bad_mem += b;
    bad_vu += vu_check(0x1000);
    bad_asid += asid() != my_asid;
    rounds++;
    mx_thread_yield();
  }
  done = 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k31: usage: k31_beside.elf <program> [args]\n");
    return t_done("k31");
  }
  for (u32 i = 0; i < N; i++)
    pattern[i] = i * 2654435761u;
  my_asid = asid();
  u32 off = mx__core_off;
  CHECK(off != 0);
  mx_irq_intc(MX_INTC_VBON, vblank, 0);
  CHECK(mx_thread_start(worker, 0, stack, sizeof stack, 120) >= 0);

  static char args[256];
  u32 n = 0;
  for (int i = 1; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  mx_printf("k31: core offset %08x, ASID %u; starting %s beside\n", off, my_asid, argv[1]);
  u32 r0 = rounds, v0 = vblanks;
  int st = mx_loader_launch_beside(argc - 1, args, n, 0);
  u32 r = rounds - r0, v = vblanks - v0;
  /* the worker ends (it is below this thread) */
  stop = 1;
  mx_thread_set_prio(mx_thread_self(), 127);
  while (!done)
    mx_thread_yield();
  mx_irq_intc(MX_INTC_VBON, 0, 0);
  mx_printf("k31: back, status %d; meanwhile %u rounds of the worker, %u vblanks here; "
            "changed: %u words, %u VU0 registers, %u ASIDs\n",
            st, r, v, bad_mem, bad_vu, bad_asid);
  CHECK_EQ(st, 0);
  CHECK(r > 0);
  CHECK(v > 0); /* its handler ran while the other did */
  CHECK_EQ(bad_mem, 0);
  CHECK_EQ(bad_vu, 0);
  CHECK_EQ(bad_asid, 0);
  CHECK_EQ(mx__core_off, off);
  CHECK_EQ(asid(), my_asid);
  return t_done("k31");
}
