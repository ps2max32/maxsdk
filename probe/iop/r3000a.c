/* The IOP core on real hardware: which CPU it is, what its instructions do,
   what its hazards really do and what they cost. Results in docs/iop-core.md.

     r3000a.irx        id, results, hazards, timing
     r3000a.irx inst   every instruction on its own, and the hazards
     r3000a.irx exc    id, which opcodes trap and what a trap costs (replaces the
                       exception vector for a moment, with interrupts off)

   Nothing here counts on a PS2 with a Deckard: there the IOP is emulated, so
   the module stops if it finds one. Times are in ticks of the 36.864 MHz
   system clock (RTC3, 32 bits, no prescaler), the IOP's own clock. */

#include "irx_imports.h"

IRX_ID("r3000a", 1, 0);

typedef void kernel(u32 a, u32 b, u32 n, void *p);
typedef u32 fn(u32 a, u32 b);

#define KERNELS(X)                                                                         \
  X(k_empty) X(k_nop) X(k_addu) X(k_sll) X(k_lw) X(k_lb) X(k_lw_nop) X(k_lw_use)           \
  X(k_lw_nop_use) X(k_lwl_lwr) X(k_sw) X(k_sb) X(k_sw_n1) X(k_sw_n3) X(k_sw_n7) X(k_sw_lw) \
  X(k_btaken) X(k_bnot) X(k_mfc0) X(k_mfhi) X(k_mtlo) X(k_mult) X(k_multu) X(k_div)        \
  X(k_divu) X(k_mult_mflo) X(k_multu_mflo) X(k_div_mflo) X(k_divu_mflo) X(k_mult_mfhi)     \
  X(k_mult_n4) X(k_mult_n8) X(k_mult_n16) X(k_div_n16) X(k_div_n48) X(k_mult_mtlo)         \
  X(k_mult_div) X(k_sled) X(k_walk) X(k_walk_sw) X(k_store_load)
#define DECL(n) extern kernel n;
KERNELS(DECL)

extern u32 sled_start[], sled_end[], trap_entry[];
u32 trap_cause, trap_epc;

static int timer, fails;

/* udptty sends a packet per write: give it time */
#define P(...)                                                                             \
  do {                                                                                     \
    printf(__VA_ARGS__);                                                                   \
    DelayThread(2000);                                                                     \
  } while (0)

static u32 ticks(kernel *k, u32 a, u32 b, u32 n, void *p) {
  u32 best = ~0u;
  for (int r = 0; r < 6; r++) {
    int st;
    CpuSuspendIntr(&st);
    u32 t0 = GetTimerCounter(timer);
    k(a, b, n, p);
    u32 t1 = GetTimerCounter(timer);
    CpuResumeIntr(st);
    if (t1 - t0 < best)
      best = t1 - t0;
  }
  return best;
}

/* hundredths of a cycle per repetition of the body */
static u32 cost(kernel *k, u32 a, u32 b, u32 n, void *p) {
  return (ticks(k, a, b, n, p) - ticks(k_empty, a, b, n, p)) * 100 / (n * 16);
}

static void show(const char *name, u32 c) { P("t %-22s %u.%02u\n", name, c / 100, c % 100); }

#define T(k, a, b, n, p) show(#k, cost(k, a, b, n, p))

static u32 ram[64] __attribute__((aligned(64)));

static void timing(void) {
  u32 *un = (u32 *)((u32)ram | 0xa0000000);
  u32 *rom = (u32 *)0xbfc00000;
  u32 *hw = (u32 *)0xbf801070;

  T(k_nop, 0, 0, 2000, ram);
  T(k_addu, 1, 0, 2000, ram);
  T(k_sll, 1, 0, 2000, ram);
  T(k_btaken, 0, 0, 1000, ram);
  T(k_bnot, 0, 0, 1000, ram);
  T(k_mfc0, 0, 0, 1000, ram);
  T(k_mfhi, 0, 0, 1000, ram);
  T(k_mtlo, 1, 0, 1000, ram);

  P("# loads and stores, cached address of the RAM\n");
  T(k_lw, 0, 0, 500, ram);
  T(k_lb, 0, 0, 500, ram);
  T(k_lw_nop, 0, 0, 500, ram);
  T(k_lw_use, 0, 0, 500, ram);
  T(k_lw_nop_use, 0, 0, 500, ram);
  T(k_lwl_lwr, 0, 0, 500, ram);
  T(k_sw, 1, 0, 500, ram);
  T(k_sb, 1, 0, 500, ram);
  T(k_sw_n1, 1, 0, 500, ram);
  T(k_sw_n3, 1, 0, 500, ram);
  T(k_sw_n7, 1, 0, 500, ram);
  T(k_sw_lw, 1, 0, 500, ram);
  P("# the same at the uncached address\n");
  T(k_lw, 0, 0, 500, un);
  T(k_lw_nop, 0, 0, 500, un);
  T(k_sw, 1, 0, 500, un);
  T(k_sw_n7, 1, 0, 500, un);
  T(k_sw_lw, 1, 0, 500, un);
  P("# BIOS ROM, I_STAT\n");
  T(k_lw, 0, 0, 100, rom);
  T(k_lw, 0, 0, 100, hw);

  static const u32 v[] = {0,          1,          0x7ff,      0x800,      0xfffff,
                          0x100000,   0x7fffffff, 0xffffffff, 0x80000000, 0xfffff801,
                          0xfffff800, 0xfffff7ff, 0xfff00001, 0xfff00000, 0xffefffff};
  P("# mult + mflo, multu + mflo over rs, rt: cycles\n");
  for (u32 i = 0; i < sizeof v / sizeof *v; i++)
    for (u32 j = 0; j < sizeof v / sizeof *v; j++) {
      u32 c = cost(k_mult_mflo, v[i], v[j], 200, ram), d = cost(k_multu_mflo, v[i], v[j], 200, ram);
      P("m %08x %08x %u.%02u %u.%02u\n", v[i], v[j], c / 100, c % 100, d / 100, d % 100);
    }
  P("# div + mflo, divu + mflo over rs, rt\n");
  for (u32 i = 0; i < sizeof v / sizeof *v; i++)
    for (u32 j = 0; j < sizeof v / sizeof *v; j++) {
      u32 c = cost(k_div_mflo, v[i], v[j], 100, ram), d = cost(k_divu_mflo, v[i], v[j], 100, ram);
      P("d %08x %08x %u.%02u %u.%02u\n", v[i], v[j], c / 100, c % 100, d / 100, d % 100);
    }
  P("# with large operands: overlap and back to back\n");
  u32 A = 0x7fffffff, B = 0x7ffffff1;
  T(k_mult, A, B, 200, ram);
  T(k_multu, A, B, 200, ram);
  T(k_div, A, 3, 100, ram);
  T(k_divu, A, 3, 100, ram);
  T(k_mult_mfhi, A, B, 200, ram);
  T(k_mult_n4, A, B, 200, ram);
  T(k_mult_n8, A, B, 200, ram);
  T(k_mult_n16, A, B, 200, ram);
  T(k_div_n16, A, 3, 100, ram);
  T(k_div_n48, A, 3, 100, ram);
  T(k_mult_mtlo, A, B, 200, ram);
  T(k_mult_div, A, 3, 100, ram);

  P("# instruction fetch: the same loop at the three addresses of the RAM\n");
  for (u32 s = 0; s < 3; s++) {
    static const u32 seg[] = {0, 0x80000000, 0xa0000000};
    kernel *k = (kernel *)(((u32)k_nop & 0x1fffffff) | seg[s]);
    kernel *e = (kernel *)(((u32)k_empty & 0x1fffffff) | seg[s]);
    u32 n = 100;
    u32 c = (ticks(k, 0, 0, n, ram) - ticks(e, 0, 0, n, ram)) * 100 / (n * 16);
    P("t fetch_%08x %u.%02u\n", seg[s], c / 100, c % 100);
  }
  P("# I-cache: a loop of nops of this many bytes, ticks per pass, 50 passes\n");
  static const u32 sz[] = {512, 1024, 2048, 3072, 3840, 4096, 4608, 6144, 8192, 12288, 16384};
  for (u32 i = 0; i < sizeof sz / sizeof *sz; i++) {
    u32 t = ticks(k_sled, 0, 0, 50, (u8 *)sled_end - sz[i]);
    P("s %5u %u.%02u\n", sz[i], t / 50, t % 50 * 2);
  }
}

/* cycles per access, in hundredths */
static u32 walk(kernel *k, u32 stride, u32 bytes, void *p) {
  u32 n = 40960 / bytes + 2;
  return ticks(k, stride, bytes, n, p) * 100 / (n * (bytes / stride));
}

static u8 big[32768] __attribute__((aligned(4096)));

static void dcache(void) {
  static const u32 sz[] = {256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
  static const u32 st[] = {4, 8, 16, 32, 64};
  P("# D-cache: lw, stride 16, by bytes walked (4 instructions per access)\n");
  for (u32 i = 0; i < sizeof sz / sizeof *sz; i++) {
    u32 c = walk(k_walk, 16, sz[i], big);
    P("w %5u %u.%02u\n", sz[i], c / 100, c % 100);
  }
  P("# the same by stride, 32 KiB\n");
  for (u32 i = 0; i < sizeof st / sizeof *st; i++) {
    u32 c = walk(k_walk, st[i], 32768, big);
    P("v %5u %u.%02u\n", st[i], c / 100, c % 100);
  }
  P("# stores, then store + load of the same word, stride 16, 32 KiB\n");
  u32 c = walk(k_walk_sw, 16, 32768, big), d = walk(k_store_load, 16, 32768, big);
  P("y sw %u.%02u sw+lw %u.%02u\n", c / 100, c % 100, d / 100, d % 100);
  P("# the same in 512 bytes: after the first pass all hits if a store or a load allocates\n");
  c = walk(k_walk_sw, 16, 512, big), d = walk(k_store_load, 16, 512, big);
  P("y sw %u.%02u sw+lw %u.%02u\n", c / 100, c % 100, d / 100, d % 100);
  P("# uncached, for comparison\n");
  c = walk(k_walk, 16, 512, (void *)((u32)big | 0xa0000000));
  P("y lw %u.%02u\n", c / 100, c % 100);
}

/* RAM at 0x1f800000? Patterns read back, inside its first KiB only: reads from
   0x400 on are bus errors (the trap stage shows that, under its own vector;
   here they would go to ioptrap and end the run). */
static void scratchpad(void) {
  volatile u32 *sp = (volatile u32 *)0x1f800000;
  int st;
  u32 w0, w1, a, b, a2, b2, c, d;
  CpuSuspendIntr(&st);
  w0 = sp[0];
  w1 = sp[255];
  sp[0] = 0x5a5aa5a5;
  sp[255] = 0x12345678;
  a = sp[0];
  b = sp[255];
  sp[0] = 0xa5a55a5a;
  sp[255] = 0x87654321;
  a2 = sp[0];
  b2 = sp[255];
  sp[0] = w0;
  sp[255] = w1;
  CpuResumeIntr(st);
  P("p first word %08x: wrote 5a5aa5a5 a5a55a5a, read %08x %08x\n", w0, a, a2);
  P("p last word %08x: wrote 12345678 87654321, read %08x %08x\n", w1, b, b2);
  c = cost(k_lw, 0, 0, 500, (void *)sp);
  d = walk(k_walk, 16, 1024, (void *)sp);
  P("p lw %u.%02u, walk of 1 KiB %u.%02u\n", c / 100, c % 100, d / 100, d % 100);
}

extern u32 s_gap0(u32), s_gap1(u32), s_gap2(u32);

/* The manual's HI/LO hazard, with interrupts on. Between the rounds the other
   threads run: the network must not time out. */
static void hilo_stress(void) {
  u32 (*const s[3])(u32) = {s_gap0, s_gap1, s_gap2};
  u32 bad[3] = {0, 0, 0};
  for (int r = 0; r < 12; r++)
    for (int g = 0; g < 3; g++) {
      bad[g] += s[g](300000);
      DelayThread(20000);
    }
  P("z mflo, next mult at distance 0/1/2 instructions: %u %u %u wrong of 3600000 each\n", bad[0],
    bad[1], bad[2]);
}

/* ---- every instruction on its own: stage "inst" ---- */

#define EACH(X) \
  X(k_add_t) X(k_add_c) X(k_addu_t) X(k_addu_c) X(k_sub_t) X(k_sub_c) X(k_subu_t)  \
  X(k_subu_c) X(k_and_t) X(k_and_c) X(k_or_t) X(k_or_c) X(k_xor_t) X(k_xor_c)  \
  X(k_nor_t) X(k_nor_c) X(k_slt_t) X(k_slt_c) X(k_sltu_t) X(k_sltu_c) X(k_sllv_t)  \
  X(k_sllv_c) X(k_srlv_t) X(k_srlv_c) X(k_srav_t) X(k_srav_c) X(k_addi_t) X(k_addi_c)  \
  X(k_addiu_t) X(k_addiu_c) X(k_slti_t) X(k_slti_c) X(k_sltiu_t) X(k_sltiu_c)  \
  X(k_andi_t) X(k_andi_c) X(k_ori_t) X(k_ori_c) X(k_xori_t) X(k_xori_c) X(k_lui_t)  \
  X(k_sll_t) X(k_sll_c) X(k_srl_t) X(k_srl_c) X(k_sra_t) X(k_sra_c) X(k_sll31_t)  \
  X(k_lb_t) X(k_lbu_t) X(k_lh_t) X(k_lhu_t) X(k_lw_t) X(k_lwl_t) X(k_lwr_t)  \
  X(k_lh_odd2) X(k_lwl_o1) X(k_lwr_o1) X(k_sb_t) X(k_sb_n) X(k_sh_t) X(k_sh_n)  \
  X(k_sw_t) X(k_sw_n) X(k_swl_t) X(k_swl_n) X(k_swr_t) X(k_swr_n) X(k_beq_y) X(k_beq_n)  \
  X(k_bne_y) X(k_bne_n) X(k_blez_y) X(k_blez_n) X(k_bgtz_y) X(k_bgtz_n) X(k_bltz_y)  \
  X(k_bltz_n) X(k_bgez_y) X(k_bgez_n) X(k_bltzal_y) X(k_bltzal_n) X(k_bgezal_y)  \
  X(k_bgezal_n) X(k_j_y) X(k_jal_y) X(k_jr_la) X(k_jalr_la) X(k_call_ret) X(k_mflo_i)  \
  X(k_mfhi_i) X(k_mtlo_i) X(k_mthi_i) X(k_mfc0_sr) X(k_mfc0_cause) X(k_mfc0_epc)  \
  X(k_mfc0_prid) X(k_alu_addr) X(k_alu_branch) X(k_alu_store) X(k_lw_nop_sw)  \
  X(k_mflo_use) X(k_mtlo_mflo) X(k_mtc0_sr) X(k_rfe) 
#define DECL2(n) extern kernel n;
EACH(DECL2)
extern kernel k_trap_syscall, k_trap_break, k_trap_ri, k_trap_cpu;

static void each(void) {
  /* a0 positive, a1 = -1 for the branches and small for the shifts: see r3000a_asm.S */
#define ONE(n)                                                                             \
  do {                                                                                     \
    u32 c = cost(n, 0x1234, 0xffffffff, 1000, ram);                                        \
    P("i %-16s %u.%02u\n", &#n[2], c / 100, c % 100);                                    \
  } while (0);
  EACH(ONE)
  P("# variable shifts by the amount: 0, 1, 31\n");
  static const u32 am[] = {0, 1, 31};
  for (u32 i = 0; i < 3; i++) {
    u32 a = cost(k_sllv_t, 0x1234, am[i], 1000, ram), b = cost(k_srlv_t, 0x1234, am[i], 1000, ram);
    u32 d = cost(k_srav_t, 0x1234, am[i], 1000, ram);
    P("i shift by %2u: sllv %u.%02u srlv %u.%02u srav %u.%02u\n", am[i], a / 100, a % 100, b / 100,
      b % 100, d / 100, d % 100);
  }
  P("# stores to the uncached address\n");
  u32 *un = (u32 *)((u32)ram | 0xa0000000);
  u32 a = cost(k_sb_n, 1, 0, 500, un), b = cost(k_sh_n, 1, 0, 500, un), d = cost(k_swl_n, 1, 0, 500, un);
  P("i uncached sb+nop %u.%02u sh+nop %u.%02u swl+nop %u.%02u\n", a / 100, a % 100, b / 100, b % 100,
    d / 100, d % 100);
  P("# loads from the uncached address\n");
  a = cost(k_lb_t, 0, 0, 500, un), b = cost(k_lh_t, 0, 0, 500, un), d = cost(k_lwl_t, 0, 0, 500, un);
  P("i uncached lb %u.%02u lh %u.%02u lwl %u.%02u\n", a / 100, a % 100, b / 100, b % 100, d / 100,
    d % 100);
}

/* ---- results ---- */

#define F(n) extern fn f_##n;
#define FS(X)                                                                              \
  X(add) X(addu) X(sub) X(subu) X(and) X(or) X(xor) X(nor) X(slt) X(sltu) X(sllv) X(srlv)  \
  X(srav) X(addi) X(addiu) X(slti) X(sltiu) X(andi) X(ori) X(xori) X(lui) X(sll) X(srl)    \
  X(sra) X(mult_lo) X(mult_hi) X(multu_lo) X(multu_hi) X(div_lo) X(div_hi) X(divu_lo)      \
  X(divu_hi) X(mtlo) X(mthi) X(lb0) X(lb1) X(lbu1) X(lh0) X(lh2) X(lhu2) X(lw0) X(lwl0)    \
  X(lwl1) X(lwl2) X(lwl3) X(lwr0) X(lwr1) X(lwr2) X(lwr3) X(sb1) X(sh2) X(sw0) X(swl0)     \
  X(swl1) X(swl2) X(swl3) X(swr0) X(swr1) X(swr2) X(swr3) X(beq) X(bne) X(blez) X(bgtz)    \
  X(bltz) X(bgez) X(bltzal) X(bgezal) X(jal) X(jalr) X(j) X(jr)
FS(F)

static const struct {
  const char *name;
  fn *f;
  u32 a, b, want;
} alu[] = {
#define A(n, a, b, w) {#n, f_##n, a, b, w},
    A(add, 0x7ffffff0, 0xf, 0x7fffffff) A(addu, 0x7fffffff, 1, 0x80000000)
    A(sub, 5, 7, 0xfffffffe) A(subu, 0x80000000, 1, 0x7fffffff)
    A(and, 0xff00ff00, 0x0ff00ff0, 0x0f000f00) A(or, 0xff00ff00, 0x0ff00ff0, 0xfff0fff0)
    A(xor, 0xff00ff00, 0x0ff00ff0, 0xf0f0f0f0) A(nor, 0xff00ff00, 0x0ff00ff0, 0x000f000f)
    A(slt, 0xffffffff, 0, 1) A(slt, 0, 0xffffffff, 0) A(sltu, 0xffffffff, 0, 0)
    A(sltu, 0, 0xffffffff, 1)
    A(sllv, 0x80000001, 33, 2) A(srlv, 0x80000000, 33, 0x40000000)
    A(srav, 0x80000000, 33, 0xc0000000)
    A(addi, 5, 0, 3) A(addiu, 0, 0, 0xfffffffe) A(slti, 0xfffffffd, 0, 1) A(slti, 5, 0, 0)
    A(sltiu, 0xfffffffd, 0, 1) A(sltiu, 0xffffffff, 0, 0)
    A(andi, 0xffffffff, 0, 0xf0f0) A(ori, 0xffff0000, 0, 0xfffff0f0)
    A(xori, 0xffffffff, 0, 0xffff0f0f) A(lui, 0, 0, 0xabcd0000)
    A(sll, 0x8000000f, 0, 0xf0) A(srl, 0x80000000, 0, 0x08000000)
    A(sra, 0x80000000, 0, 0xf8000000)
    A(mult_lo, 0xffffffff, 0x7fffffff, 0x80000001) A(mult_hi, 0xffffffff, 0x7fffffff, 0xffffffff)
    A(multu_lo, 0xffffffff, 0xffffffff, 1) A(multu_hi, 0xffffffff, 0xffffffff, 0xfffffffe)
    A(div_lo, 0xfffffff9, 2, 0xfffffffd) A(div_hi, 0xfffffff9, 2, 0xffffffff)
    A(divu_lo, 0xfffffff9, 2, 0x7ffffffc) A(divu_hi, 0xfffffff9, 2, 1)
    A(mtlo, 0x12345678, 0, 0x12345678) A(mthi, 0x87654321, 0, 0x87654321)
    A(beq, 3, 3, 3) A(beq, 3, 4, 2) A(bne, 3, 3, 2) A(bne, 3, 4, 3)
    A(blez, 0, 0, 3) A(blez, 1, 0, 2) A(blez, 0xffffffff, 0, 3)
    A(bgtz, 0, 0, 2) A(bgtz, 1, 0, 3) A(bgtz, 0xffffffff, 0, 2)
    A(bltz, 0, 0, 2) A(bltz, 0xffffffff, 0, 3) A(bgez, 0, 0, 3) A(bgez, 0xffffffff, 0, 2)
    A(bltzal, 0, 0, 8) A(bgezal, 0, 0, 8) A(jal, 0, 0, 8) A(jalr, 0, 0, 8) A(j, 0, 0, 1)
    A(jr, 0, 0, 1)
};

static const struct {
  const char *name;
  fn *f;
  u32 want;
} loads[] = {
#define L(n, w) {#n, f_##n, w},
    L(lb0, 0xffffff81) L(lb1, 0x00000072) L(lbu1, 0x00000072) L(lh0, 0x00007281 | 0)
    L(lh2, 0xffff8483) L(lhu2, 0x00008483) L(lw0, 0x84837281)
    L(lwl0, 0x81bbccdd) L(lwl1, 0x7281ccdd) L(lwl2, 0x837281dd) L(lwl3, 0x84837281)
    L(lwr0, 0x84837281) L(lwr1, 0xaa848372) L(lwr2, 0xaabb8483) L(lwr3, 0xaabbcc84)
};

static const struct {
  const char *name;
  fn *f;
  u32 want;
} stores[] = {
    L(sb1, 0x4433dd11) L(sh2, 0xccdd2211) L(sw0, 0xaabbccdd)
    L(swl0, 0x443322aa) L(swl1, 0x4433aabb) L(swl2, 0x44aabbcc) L(swl3, 0xaabbccdd)
    L(swr0, 0xaabbccdd) L(swr1, 0xbbccdd11) L(swr2, 0xccdd2211) L(swr3, 0xdd332211)
};

static void check(const char *name, u32 got, u32 want) {
  if (got != want) {
    P("f FAIL %s: %08x, expected %08x\n", name, got, want);
    fails++;
  }
}

static void results(void) {
  for (u32 i = 0; i < sizeof alu / sizeof *alu; i++)
    check(alu[i].name, alu[i].f(alu[i].a, alu[i].b), alu[i].want);
  ram[0] = 0x84837281;
  for (u32 i = 0; i < sizeof loads / sizeof *loads; i++)
    check(loads[i].name, loads[i].f((u32)ram, 0xaabbccdd), loads[i].want);
  for (u32 i = 0; i < sizeof stores / sizeof *stores; i++) {
    ram[0] = 0x44332211;
    check(stores[i].name, stores[i].f((u32)ram, 0xaabbccdd), stores[i].want);
  }
  P("f %u checks, %d failed\n",
    (u32)(sizeof alu / sizeof *alu + sizeof loads / sizeof *loads + sizeof stores / sizeof *stores),
    fails);
  /* what the manual leaves undefined */
  P("u div 7/0: lo %08x hi %08x\n", f_div_lo(7, 0), f_div_hi(7, 0));
  P("u div -7/0: lo %08x hi %08x\n", f_div_lo(-7, 0), f_div_hi(-7, 0));
  P("u divu 7/0: lo %08x hi %08x\n", f_divu_lo(7, 0), f_divu_hi(7, 0));
  P("u div min/-1: lo %08x hi %08x\n", f_div_lo(0x80000000, -1), f_div_hi(0x80000000, -1));
}

/* ---- hazards ---- */

#define HS(X)                                                                              \
  X(h_load_use) X(h_load_nop_use) X(h_load_write) X(h_lb_write) X(h_load_load)             \
  X(h_load_base) X(h_load_load_use) X(h_lw_lwl) X(h_lwl_lwr) X(h_lwr_use) X(h_load_branch) \
  X(h_load_jr) X(h_load_bdelay) X(h_load_bdelay_nop) X(h_mfc0_use) X(h_mfc0_nop_use)       \
  X(h_jal_ra) X(h_mult_mtlo) X(h_mult_mthi_lo) X(h_mult_mthi_hi) X(h_mult_mult)            \
  X(h_mult_div) X(h_div_mult) X(h_mflo_mult) X(h_mult_mtlo_hi) X(h_old_mult_mthi_lo)        \
  X(h_mult_n3_mthi_lo) X(h_mult_n20_mthi_lo) X(h_div_mthi_lo) X(h_div_mtlo_hi)
#define H(n) extern u32 n(u32 *d);
HS(H)

extern u32 c0_sr(void), h_mflo_use(void), h_mfhi_use(void);
extern u32 h_mtc0_d0(void), h_mtc0_d1(void), h_mtc0_d2(void), h_mtc0_d3(void);

static void hazards(void) {
  static u32 other[2] = {0, 0x3333};
  u32 d[8] = {0x1111, 0x2222, (u32)other, 0, 0, 0x44332211, 0x88776655, 0};
#define SHOW(n) P("h %-20s %08x\n", #n, n(d));
  HS(SHOW)
  P("h h_mflo_use %08x h_mfhi_use %08x\n", h_mflo_use(), h_mfhi_use());
  /* interrupts off: SR changes mask bits */
  int st;
  CpuSuspendIntr(&st);
  u32 sr = c0_sr(), m0 = h_mtc0_d0(), m1 = h_mtc0_d1(), m2 = h_mtc0_d2(), m3 = h_mtc0_d3();
  CpuResumeIntr(st);
  P("h mtc0 SR %08x then mfc0 at distance 0/1/2/3: %08x %08x %08x %08x\n", sr, m0, m1, m2, m3);
}

/* ---- traps ---- */

extern void x_none(u8 *), x_add_ov(u8 *), x_addi_ov(u8 *), x_sub_ov(u8 *), x_lw_unal(u8 *),
    x_lh_unal(u8 *), x_sw_unal(u8 *), x_sh_unal(u8 *), x_syscall(u8 *), x_break(u8 *),
    x_scratch(u8 *), x_tlbp(u8 *), x_tlbr(u8 *), x_tlbwi(u8 *), x_tlbwr(u8 *), x_bc1t(u8 *),
    x_cfc1(u8 *), x_ctc1(u8 *), x_bc2f(u8 *), x_bc2t(u8 *), x_cache(u8 *), x_pref(u8 *), x_swc3(u8 *),
    x_mfc1(u8 *), x_mtc1(u8 *), x_lwc1(u8 *), x_swc1(u8 *), x_add_s(u8 *), x_bc1f(u8 *),
    x_mfc2(u8 *), x_mtc2(u8 *), x_cfc2(u8 *), x_ctc2(u8 *), x_lwc2(u8 *), x_swc2(u8 *),
    x_cop2(u8 *), x_bc0f(u8 *), x_bc0t(u8 *), x_ll(u8 *), x_sc(u8 *), x_sync(u8 *), x_teq(u8 *),
    x_beql(u8 *), x_ldc1(u8 *), x_movz(u8 *), x_mul(u8 *), x_clz(u8 *), x_daddu(u8 *), x_ld(u8 *),
    x_sd(u8 *), x_res_op(u8 *), x_res_fn(u8 *);

static const struct {
  const char *name;
  void (*f)(u8 *);
} traps[] = {
#define X(n) {#n, x_##n},
    X(none) X(add_ov) X(addi_ov) X(sub_ov) X(lw_unal) X(lh_unal) X(sw_unal) X(sh_unal) X(syscall)
    X(break) X(scratch) X(tlbp) X(tlbr) X(tlbwi) X(tlbwr) X(bc1t) X(cfc1) X(ctc1) X(bc2f) X(bc2t)
    X(cache) X(pref) X(swc3) X(mfc1) X(mtc1) X(lwc1) X(swc1)
    X(add_s) X(bc1f) X(mfc2) X(mtc2) X(cfc2) X(ctc2) X(lwc2) X(swc2) X(cop2) X(bc0f) X(bc0t) X(ll)
    X(sc) X(sync) X(teq) X(beql) X(ldc1) X(movz) X(mul) X(clz) X(daddu) X(ld) X(sd) X(res_op)
    X(res_fn)
};
#define NTRAPS (sizeof traps / sizeof *traps)

/* the end of the region at 0x1f800000 */
static const u32 peek[] = {0x1f800000, 0x1f8003fc, 0x1f800400, 0x1f800800, 0x1f801000 - 4};
#define NPEEK (sizeof peek / sizeof *peek)
extern u32 x_peek(u32 addr);

static void exceptions(void) {
  static u32 cause[NTRAPS], peeked[NPEEK], peek_cause[NPEEK], peek_epc[NPEEK];
  static u32 buf[32] __attribute__((aligned(64)));
  volatile u32 *vec = (volatile u32 *)0x80;
  int st;

  CpuSuspendIntr(&st);
  /* flushed first: FlushIcache must not depend on the vector being replaced */
  FlushIcache();
  u32 v0 = vec[0], v1 = vec[1];
  vec[0] = 0x08000000 | (((u32)trap_entry >> 2) & 0x3ffffff);
  vec[1] = 0;
  for (u32 i = 0; i < NTRAPS; i++) {
    trap_cause = 0;
    traps[i].f((u8 *)&buf[8]);
    cause[i] = trap_cause;
  }
  for (u32 i = 0; i < NPEEK; i++) {
    trap_cause = 0;
    peeked[i] = x_peek(peek[i]);
    peek_cause[i] = trap_cause;
    peek_epc[i] = trap_epc;
  }
  static kernel *const tk[4] = {k_trap_syscall, k_trap_break, k_trap_ri, k_trap_cpu};
  u32 tt[5];
  if (timer > 0) {
    for (u32 i = 0; i < 4; i++) {
      u32 t0 = GetTimerCounter(timer);
      tk[i](0, 0, 20, buf);
      tt[i] = GetTimerCounter(timer) - t0;
    }
    u32 t0 = GetTimerCounter(timer);
    k_empty(0, 0, 20, buf);
    tt[4] = GetTimerCounter(timer) - t0;
  }
  vec[0] = v0;
  vec[1] = v1;
  trap_cause = 0;
  FlushIcache();
  u32 after = trap_cause;
  CpuResumeIntr(st);

  for (u32 i = 0; i < NTRAPS; i++)
    P("x %-10s cause %08x exccode %u ce %u\n", traps[i].name, cause[i], (cause[i] >> 2) & 31,
      (cause[i] >> 28) & 3);
  for (u32 i = 0; i < NPEEK; i++)
    P("x peek %08x: %08x cause %08x exccode %u epc-peek %d\n", peek[i], peeked[i], peek_cause[i],
      (peek_cause[i] >> 2) & 31, (int)(peek_epc[i] - (u32)x_peek));
  P("x trap during the flush: %08x\n", after);
  if (timer > 0)
    P("x round trip, ticks of 320, loop %u: syscall %u break %u reserved %u cop unusable %u\n",
      tt[4], tt[0], tt[1], tt[2], tt[3]);
}

extern u32 c0_prid(void);

int _start(int argc, char **argv) {
  u32 gm_if = *(volatile u32 *)0xbf801450;
  u32 prid = c0_prid(), sr = c0_sr();
  u32 biu = *(volatile u32 *)0xfffe0130;
  P("r3000a: GM_IF %08x PRId %08x SR %08x BIU %08x\n", gm_if, prid, sr, biu);
  if (gm_if & 0x80000000) {
    P("r3000a: this IOP is emulated (Deckard): no results\n");
    return 1 << 2 | MODULE_NO_RESIDENT_END;
  }

  timer = AllocHardTimer(1, 32, 1);
  P("r3000a: timer %d\n", timer);
  if (timer < 0)
    return 3 << 2 | MODULE_NO_RESIDENT_END;
  SetTimerMode(timer, 0);
  u32 t0 = GetTimerCounter(timer);
  DelayThread(1000);
  u32 t1 = GetTimerCounter(timer);
  P("r3000a: 1 ms of DelayThread: %u ticks\n", t1 - t0);
  if (t1 == t0)
    return 4 << 2 | MODULE_NO_RESIDENT_END;

  if (argc > 1 && argv[1][0] == 'i') {
    each();
    hazards();
    FreeHardTimer(timer);
    P("r3000a: end\n");
    return MODULE_NO_RESIDENT_END;
  }
  if (argc > 1) {
    if (sr & (1 << 22)) {
      P("r3000a: BEV is set, the vector is not at 0x80\n");
      FreeHardTimer(timer);
      return 2 << 2 | MODULE_NO_RESIDENT_END;
    }
    exceptions();
    FreeHardTimer(timer);
    P("r3000a: end\n");
    return MODULE_NO_RESIDENT_END;
  }

  results();
  hazards();
  timing();
  dcache();
  scratchpad();
  hilo_stress();
  FreeHardTimer(timer);
  P("r3000a: end, %d failed\n", fails);
  return fails << 2 | MODULE_NO_RESIDENT_END;
}
