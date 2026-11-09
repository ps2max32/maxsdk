/* Step 2e-2 (docs/mx-os-apps-plan.md): the program in front while another
   process runs beside it (k31_beside): each time it waits the other's thread
   runs, and its memory in VM and in core, its ASID, VU0's registers and its
   own interrupt handlers (a vblank counter, mx_sleep_us's timer) stay its
   own. 300 rounds of 5 ms. */
#include "t.h"
#include <mx/irq.h>
#include <mx/thread.h>

#define N 4096
static u32 vm_buf[N];
static u32 core_buf[N] MX_CORE_DATA;
static volatile u32 vblanks MX_CORE_DATA;

static u32 entry_hi(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v;
}

static u32 asid(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v & 0xFFu;
}

MX_CORE_FN static void vblank(void *arg) {
  (void)arg;
  vblanks++;
}

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

int main(void) {
  for (u32 i = 0; i < N; i++)
    vm_buf[i] = i * 40503u + 7, core_buf[i] = ~(i * 40503u);
  u32 a = asid(), bad_mem = 0, bad_vu = 0, bad_asid = 0;
  mx_printf("vm32: ASID %u, core offset %08x\n", a, mx__core_off);
  mx_irq_intc(MX_INTC_VBON, vblank, 0);
  vu_set(0x2000);
  for (u32 r = 0; r < 300; r++) {
    mx_sleep_us(5000);
    for (u32 i = 0; i < N; i++)
      bad_mem += vm_buf[i] != i * 40503u + 7 || core_buf[i] != ~(i * 40503u);
    bad_vu += vu_check(0x2000);
    if (asid() != a && !bad_asid)
      mx_printf("vm32: round %u, ASID %u, EntryHi %08x\n", r, asid(), entry_hi());
    bad_asid += asid() != a;
    if (r % 100 == 99)
      mx_printf("vm32: round %u, %u vblanks\n", r + 1, vblanks);
  }
  mx_irq_intc(MX_INTC_VBON, 0, 0);
  mx_printf("vm32: %u vblanks; changed: %u words, %u VU0 registers, %u ASIDs\n", vblanks,
            bad_mem, bad_vu, bad_asid);
  CHECK(vblanks >= 60);
  CHECK_EQ(bad_mem, 0);
  CHECK_EQ(bad_vu, 0);
  CHECK_EQ(bad_asid, 0);
  return t_done("vm32");
}
