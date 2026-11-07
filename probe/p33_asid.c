/* Probe for processes (docs/mx-os-plan.md, step 2): address spaces by ASID,
   and which registers the BIOS keeps at a thread switch.

   1. Two TLB entries for one virtual address (0x0800_0000), not global, ASID 5
      and 6, on other frames; one global entry (0x0800_8000). With EntryHi's
      ASID 5 the address reads frame A, with 6 frame B; the global one reads
      the same under 0, 5 and 6.
   2. EntryHi's ASID after a syscall, and after 10 vblanks (interrupts).
   3. A thread switch: is the ASID the thread's (kept and set again by the
      BIOS) or the CPU's (what the other thread set stays)?
   4. VU0's registers (VF1-31, VI1-15) and the FPU's (f0-31) over a thread
      switch, the other thread writing its own: when the thread switches
      itself (StartThread, WakeupThread of a higher one) and when an interrupt
      wakes a higher thread (iWakeupThread from the vblank handler) while the
      first is busy.
   The entries used (45-47) are the BIOS's unused ones; they and EntryHi are
   set back at the end (and mxlink writes the BIOS's TLB back at every start). */

#include <mx/bios.h>
#include <mx/irq.h>
#include <mx/mx.h>
#include <mx/thread.h>

#define VA 0x08000000u
#define VA_G 0x08008000u
#define PM16K 0x6000u

static u8 frame[3][2][16384] MX_ALIGNED(16384);
static int fails;

#define CHECK(c, ...)                                                                   \
  do {                                                                                  \
    if (!(c)) {                                                                         \
      mx_printf("FAIL %d: ", __LINE__);                                                 \
      mx_printf(__VA_ARGS__);                                                           \
      mx_printf("\n");                                                                  \
      fails++;                                                                          \
    }                                                                                   \
  } while (0)

MX_INLINE u32 entryhi(void) {
  u32 v;
  __asm__ volatile("mfc0 %0, $10" : "=r"(v));
  return v;
}
MX_INLINE void set_asid(u32 asid) {
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(asid & 0xFF) : "memory");
}

static void tlb_read(u32 i, u32 *pm, u32 *hi, u32 *lo0, u32 *lo1) {
  int was = mx_di();
  u32 keep = entryhi();
  __asm__ volatile("mtc0 %4, $0\n\tsync.p\n\ttlbr\n\tsync.p\n\t"
                   "mfc0 %0, $5\n\tmfc0 %1, $10\n\tmfc0 %2, $2\n\tmfc0 %3, $3"
                   : "=r"(*pm), "=r"(*hi), "=r"(*lo0), "=r"(*lo1)
                   : "r"(i));
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(keep));
  mx_irq_restore(was);
}

static void tlb_write(u32 i, u32 pm, u32 hi, u32 lo0, u32 lo1) {
  int was = mx_di();
  u32 keep = entryhi();
  __asm__ volatile("mtc0 %0, $0\n\tmtc0 %1, $5\n\tmtc0 %2, $10\n\tmtc0 %3, $2\n\tmtc0 %4, $3\n\t"
                   "sync.p\n\ttlbwi\n\tsync.p"
                   :
                   : "r"(i), "r"(pm), "r"(hi), "r"(lo0), "r"(lo1)
                   : "memory");
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(keep));
  mx_irq_restore(was);
}

/* EntryLo of a 16 KiB frame: PFN, cached (C 3), dirty, valid, global */
static u32 lo(const void *p, int g) {
  return ((u32)(uintptr_t)p >> 12) << 6 | 3u << 3 | 1u << 2 | 1u << 1 | (u32)(g ? 1 : 0);
}

static u32 rd(u32 va) { return *(volatile u32 *)(uintptr_t)va; }

/* ---- VU0 and FPU registers ------------------------------------------------ */

/* VF1-31 and VI1-15 set from base (vf n: base + n in each word, vi n: base
   + n, 16 bits), and read back into out */
#define VF_SET(n) "qmtc2 $8, $vf" #n "\n\t"
#define VF_GET(n) "qmfc2 $8, $vf" #n "\n\tsq $8, " #n "*16(%0)\n\t"

static void vu0_set(u32 base) {
  static u32 q[32][4] MX_ALIGNED(16);
  for (int n = 0; n < 32; n++)
    for (int k = 0; k < 4; k++)
      q[n][k] = base + (u32)n;
  for (int n = 1; n < 32; n++) {
    switch (n) {
#define C(x)                                                                            \
  case x:                                                                               \
    __asm__ volatile("lq $8, 0(%0)\n\t" VF_SET(x) : : "r"(q[n]) : "$8");               \
    break;
      C(1) C(2) C(3) C(4) C(5) C(6) C(7) C(8) C(9) C(10) C(11) C(12) C(13) C(14) C(15) C(16)
      C(17) C(18) C(19) C(20) C(21) C(22) C(23) C(24) C(25) C(26) C(27) C(28) C(29) C(30)
      C(31)
#undef C
    }
  }
  for (int n = 1; n < 16; n++) {
    u32 v = ((base >> 20) + (u32)n) & 0xFFFF;
    switch (n) {
#define C(x)                                                                            \
  case x:                                                                               \
    __asm__ volatile("ctc2 %0, $vi" #x : : "r"(v));                                     \
    break;
      C(1) C(2) C(3) C(4) C(5) C(6) C(7) C(8) C(9) C(10) C(11) C(12) C(13) C(14) C(15)
#undef C
    }
  }
}

/* the VF words (of 124) and VI registers (of 15) that are not as vu0_set(base)
   left them: vi_bad gets the VI count */
static int vi_bad;
static int vu0_wrong(u32 base) {
  vi_bad = 0;
  static u32 q[32][4] MX_ALIGNED(16);
  __asm__ volatile(VF_GET(1) VF_GET(2) VF_GET(3) VF_GET(4) VF_GET(5) VF_GET(6) VF_GET(7)
                       VF_GET(8) VF_GET(9) VF_GET(10) VF_GET(11) VF_GET(12) VF_GET(13)
                           VF_GET(14) VF_GET(15) VF_GET(16) VF_GET(17) VF_GET(18) VF_GET(19)
                               VF_GET(20) VF_GET(21) VF_GET(22) VF_GET(23) VF_GET(24)
                                   VF_GET(25) VF_GET(26) VF_GET(27) VF_GET(28) VF_GET(29)
                                       VF_GET(30) VF_GET(31)
                   :
                   : "r"(q)
                   : "$8", "memory");
  int bad = 0;
  for (int n = 1; n < 32; n++)
    for (int k = 0; k < 4; k++)
      bad += q[n][k] != base + (u32)n;
  u32 vi[16];
#define G(x) __asm__ volatile("cfc2 %0, $vi" #x : "=r"(vi[x]));
  G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15)
#undef G
  for (int n = 1; n < 16; n++)
    vi_bad += (vi[n] & 0xFFFF) != (((base >> 20) + (u32)n) & 0xFFFF);
  return bad;
}

static void fpu_set(u32 base) {
  static u32 w[32];
  for (int n = 0; n < 32; n++)
    w[n] = base + (u32)n;
#define F(x) "lwc1 $f" #x ", " #x "*4(%0)\n\t"
  __asm__ volatile(F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13)
                       F(14) F(15) F(16) F(17) F(18) F(19) F(20) F(21) F(22) F(23) F(24) F(25)
                           F(26) F(27) F(28) F(29) F(30) F(31)
                   :
                   : "r"(w)
                   : "memory");
#undef F
}

static int fpu_wrong(u32 base) {
  static u32 w[32];
#define F(x) "swc1 $f" #x ", " #x "*4(%0)\n\t"
  __asm__ volatile(F(0) F(1) F(2) F(3) F(4) F(5) F(6) F(7) F(8) F(9) F(10) F(11) F(12) F(13)
                       F(14) F(15) F(16) F(17) F(18) F(19) F(20) F(21) F(22) F(23) F(24) F(25)
                           F(26) F(27) F(28) F(29) F(30) F(31)
                   :
                   : "r"(w)
                   : "memory");
#undef F
  int bad = 0;
  for (int n = 0; n < 32; n++)
    bad += w[n] != base + (u32)n;
  return bad;
}

/* ---- the other thread ------------------------------------------------------ */

#define A_BASE 0x3F000000u /* VF words that are floats near 0.5: harmless */
#define B_BASE 0x40000000u

static volatile u32 b_hi_in, b_runs, b_asid_out = 6;
static int b_tid;
static u8 b_stack[16384] MX_ALIGNED(16);

static void other(void *arg) {
  (void)arg;
  for (;;) {
    b_hi_in = entryhi();
    set_asid(b_asid_out);
    vu0_set(B_BASE);
    fpu_set(B_BASE);
    b_runs++;
    mx_bios_sleep_thread();
  }
}

static volatile int irq_wake;
static void vblank(void *arg) {
  (void)arg;
  if (irq_wake) {
    irq_wake = 0;
    mx_bios_iwakeup_thread(b_tid);
  }
}

int main(void) {
  u32 keep_hi = entryhi();
  u32 save[3][4];
  for (u32 i = 0; i < 3; i++)
    tlb_read(45 + i, &save[i][0], &save[i][1], &save[i][2], &save[i][3]);
  mx_printf("p33_asid: EntryHi at the start %08x (ASID %u); entries 45-47 hi %08x %08x %08x\n",
            keep_hi, keep_hi & 0xFF, save[0][1], save[1][1], save[2][1]);

  /* 1. the frames and the entries */
  for (int f = 0; f < 3; f++)
    for (int h = 0; h < 2; h++)
      for (u32 i = 0; i < 16384 / 4; i++)
        ((volatile u32 *)frame[f][h])[i] = 0xA0000000u + (u32)f * 0x100000u + (u32)h * 0x10000u + i;
  mx_bios_flush_cache(0);
  tlb_write(47, PM16K, VA | 5, lo(frame[0][0], 0), lo(frame[0][1], 0));
  tlb_write(46, PM16K, VA | 6, lo(frame[1][0], 0), lo(frame[1][1], 0));
  tlb_write(45, PM16K, VA_G, lo(frame[2][0], 1), lo(frame[2][1], 1));
  u32 a = 0xA0000000u, b = 0xA0100000u, g = 0xA0200000u;
  set_asid(5);
  CHECK(rd(VA) == a && rd(VA + 16384 + 8) == a + 0x10002, "ASID 5 reads %08x %08x", rd(VA),
        rd(VA + 16384 + 8));
  CHECK(rd(VA_G + 4) == g + 1, "global under 5: %08x", rd(VA_G + 4));
  set_asid(6);
  CHECK(rd(VA) == b, "ASID 6 reads %08x", rd(VA));
  CHECK(rd(VA_G + 4) == g + 1, "global under 6: %08x", rd(VA_G + 4));
  set_asid(0);
  CHECK(rd(VA_G + 4) == g + 1, "global under 0: %08x", rd(VA_G + 4));
  /* stores through ASID 6 land in frame B only */
  set_asid(6);
  *(volatile u32 *)VA = 0x12345678u;
  set_asid(5);
  CHECK(rd(VA) == a, "ASID 5 after a store under 6: %08x", rd(VA));
  set_asid(6);
  CHECK(rd(VA) == 0x12345678u, "ASID 6 after its store: %08x", rd(VA));
  mx_printf("p33_asid: 1. two address spaces at %08x by ASID, global entry: %s\n", VA,
            fails ? "FAIL" : "ok");

  /* 2. a syscall, interrupts */
  int f0 = fails;
  set_asid(5);
  mx_bios_get_thread_id();
  CHECK((entryhi() & 0xFF) == 5, "ASID after a syscall: %u", entryhi() & 0xFF);
  mx_irq_intc(MX_INTC_VBON, vblank, 0);
  u32 t0 = mx_count(), bad = 0;
  while (mx_count() - t0 < MX_EE_HZ / 5) /* 200 ms: 10 vblanks and more */
    bad += (entryhi() & 0xFF) != 5 || rd(VA) != a;
  CHECK(!bad, "ASID 5 lost %u times in 200 ms of interrupts", bad);
  mx_printf("p33_asid: 2. the ASID after a syscall and 200 ms of interrupts: %s\n",
            fails == f0 ? "kept" : "FAIL");

  /* 3. and 4. thread switches */
  f0 = fails;
  int prio = mx_thread_prio(mx_thread_self());
  vu0_set(A_BASE);
  fpu_set(A_BASE);
  b_tid = mx_thread_start(other, 0, b_stack, sizeof b_stack, prio - 1);
  CHECK(b_tid >= 0 && b_runs == 1, "the other thread: %d, runs %u", b_tid, b_runs);
  u32 hi_after = entryhi() & 0xFF;
  int vu_bad = vu0_wrong(A_BASE), fpu_bad = fpu_wrong(A_BASE);
  mx_printf("p33_asid: 3. start of a higher thread: it saw ASID %u, set 6; back here ASID %u (%s)\n",
            b_hi_in & 0xFF, hi_after,
            hi_after == 5 ? "the thread's: the BIOS sets it again" : "the CPU's: not kept per thread");
  mx_printf("p33_asid: 4. after it: VF words wrong %d of 124, VI %d of 15, FPU %d of 32\n", vu_bad,
            vi_bad, fpu_bad);

  /* again by WakeupThread */
  set_asid(5);
  vu0_set(A_BASE);
  fpu_set(A_BASE);
  b_asid_out = 7;
  mx_bios_wakeup_thread(b_tid);
  CHECK(b_runs == 2, "runs %u", b_runs);
  hi_after = entryhi() & 0xFF;
  vu_bad = vu0_wrong(A_BASE), fpu_bad = fpu_wrong(A_BASE);
  mx_printf("p33_asid:    WakeupThread: it saw %u, set 7; back here %u; VF %d, VI %d, FPU %d\n",
            b_hi_in & 0xFF, hi_after, vu_bad, vi_bad, fpu_bad);

  /* preempted: the vblank handler wakes it while this thread is busy */
  set_asid(5);
  vu0_set(A_BASE);
  fpu_set(A_BASE);
  b_asid_out = 8;
  irq_wake = 1;
  t0 = mx_count();
  while (b_runs == 2 && mx_count() - t0 < MX_EE_HZ)
    ;
  CHECK(b_runs == 3, "runs %u after an interrupt's wakeup", b_runs);
  hi_after = entryhi() & 0xFF;
  vu_bad = vu0_wrong(A_BASE), fpu_bad = fpu_wrong(A_BASE);
  mx_printf("p33_asid:    by an interrupt: it saw %u, set 8; back here %u; VF %d, VI %d, FPU %d\n",
            b_hi_in & 0xFF, hi_after, vu_bad, vi_bad, fpu_bad);

  mx_irq_intc(MX_INTC_VBON, 0, 0);
  for (u32 i = 0; i < 3; i++)
    tlb_write(45 + i, save[i][0], save[i][1], save[i][2], save[i][3]);
  __asm__ volatile("mtc0 %0, $10\n\tsync.p" : : "r"(keep_hi));
  mx_printf("p33_asid: %s\n", fails ? "FAIL" : "done");
  mx_printf("@@PS2TEST_END@@ status=%d\n", fails ? 1 : 0);
  return fails ? 1 : 0;
}
