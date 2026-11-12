/* Probe (mx OS plan, step 2d): a thread switch the kernel does itself, at the
   return from an interrupt, without the BIOS's threads.

   Two threads on one BIOS thread: main and a worker with a stack of its own.
   The BIOS's INTC entry (0x8001_5380 + 8, docs/bios.md) points at
   p38_entry. A vblank start (cause 2) is taken there: acknowledged in
   I_STAT, counted, and the interrupted thread's whole context (the 128-bit
   GPRs, HI/LO, HI1/LO1, SA, EPC) saved in its record, the other's loaded,
   and ERET into it. Only from user code (EPC below 0x8000_0000): in the BIOS
   (a syscall of main's, the BIOS's idle thread) the vblank is only counted.
   Other causes go on into the BIOS's handler. After 120 vblanks main puts
   the table back. Measured: both threads ran (their counts), the switches.
   Run by mxlink (a program on libmx). */

#include <mx/bios.h>
#include <mx/mx.h>

#define TABLE 0x80015380u
#define INTC_ENTRY (TABLE + 8)
#define I_STAT 0xB000F000u
#define VBON 2

/* a thread's context: GPR i at 16 i (k0, k1, zero unused), HI/LO, HI1/LO1,
   SA, EPC after */
struct ctx {
  u32 r[32][4];
  u64 hi, lo, hi1, lo1;
  u32 sa, epc, pad[2];
} __attribute__((aligned(16)));

struct ctx p38_ctx[2];
struct ctx *p38_cur;
volatile u32 p38_vblanks, p38_switches, p38_skipped;
u32 p38_orig;
static volatile u32 count[2];
static u8 worker_stack[16 * 1024] __attribute__((aligned(16)));

void p38_entry(void);
__asm__(".set push\n.set noreorder\n.set noat\n"
        ".globl p38_entry\np38_entry:\n"
        /* a vblank start pending and enabled? */
        "  lui $26, 0xB001\n"
        "  lw $27, -0x1000($26)\n" /* 0xB000F000: I_STAT */
        "  lw $26, -0x0FF0($26)\n" /* 0xB000F010: I_MASK */
        "  and $27, $27, $26\n"
        "  andi $27, $27, 4\n"
        "  beqz $27, 9f\n"
        "  nop\n"
        /* acknowledge it, count it */
        "  lui $26, 0xB001\n"
        "  addiu $27, $0, 4\n"
        "  sw $27, -0x1000($26)\n"
        "  lui $26, %hi(p38_vblanks)\n"
        "  lw $27, %lo(p38_vblanks)($26)\n"
        "  addiu $27, $27, 1\n"
        "  sw $27, %lo(p38_vblanks)($26)\n"
        /* from user code only: else back where it was (sp, ra, at as the
           vector saved them) */
        "  mfc0 $27, $14\n"
        "  bltz $27, 8f\n"
        "  nop\n"
        /* save the interrupted thread */
        "  lui $26, %hi(p38_cur)\n"
        "  lw $26, %lo(p38_cur)($26)\n"
        "  sw $27, 0x224($26)\n" /* EPC */
        "  sq $2, 0x20($26)\n  sq $3, 0x30($26)\n  sq $4, 0x40($26)\n  sq $5, 0x50($26)\n"
        "  sq $6, 0x60($26)\n  sq $7, 0x70($26)\n  sq $8, 0x80($26)\n  sq $9, 0x90($26)\n"
        "  sq $10, 0xA0($26)\n  sq $11, 0xB0($26)\n  sq $12, 0xC0($26)\n  sq $13, 0xD0($26)\n"
        "  sq $14, 0xE0($26)\n  sq $15, 0xF0($26)\n  sq $16, 0x100($26)\n  sq $17, 0x110($26)\n"
        "  sq $18, 0x120($26)\n  sq $19, 0x130($26)\n  sq $20, 0x140($26)\n  sq $21, 0x150($26)\n"
        "  sq $22, 0x160($26)\n  sq $23, 0x170($26)\n  sq $24, 0x180($26)\n  sq $25, 0x190($26)\n"
        "  sq $28, 0x1C0($26)\n  sq $30, 0x1E0($26)\n"
        "  lui $27, 0x8000\n"
        "  lq $2, 0x10C0($27)\n  sq $2, 0x1D0($26)\n" /* sp */
        "  lq $2, 0x10D0($27)\n  sq $2, 0x1F0($26)\n" /* ra */
        "  lq $2, 0x10E0($27)\n  sq $2, 0x10($26)\n"  /* at */
        "  mfhi $2\n  sd $2, 0x200($26)\n  mflo $2\n  sd $2, 0x208($26)\n"
        "  mfhi1 $2\n  sd $2, 0x210($26)\n  mflo1 $2\n  sd $2, 0x218($26)\n"
        "  mfsa $2\n  sw $2, 0x220($26)\n"
        /* the other one */
        "  lui $27, %hi(p38_ctx)\n"
        "  addiu $27, $27, %lo(p38_ctx)\n"
        "  bne $26, $27, 1f\n"
        "  nop\n"
        "  addiu $27, $27, 0x230\n"
        "1:\n"
        "  lui $26, %hi(p38_cur)\n"
        "  sw $27, %lo(p38_cur)($26)\n"
        "  lui $26, %hi(p38_switches)\n"
        "  lw $2, %lo(p38_switches)($26)\n"
        "  addiu $2, $2, 1\n"
        "  sw $2, %lo(p38_switches)($26)\n"
        "  move $26, $27\n"
        "  lw $2, 0x220($26)\n  mtsa $2\n"
        "  ld $2, 0x210($26)\n  mthi1 $2\n  ld $2, 0x218($26)\n  mtlo1 $2\n"
        "  ld $2, 0x200($26)\n  mthi $2\n  ld $2, 0x208($26)\n  mtlo $2\n"
        "  lw $2, 0x224($26)\n  mtc0 $2, $14\n  sync.p\n"
        "  lq $1, 0x10($26)\n"
        "  lq $2, 0x20($26)\n  lq $3, 0x30($26)\n  lq $4, 0x40($26)\n  lq $5, 0x50($26)\n"
        "  lq $6, 0x60($26)\n  lq $7, 0x70($26)\n  lq $8, 0x80($26)\n  lq $9, 0x90($26)\n"
        "  lq $10, 0xA0($26)\n  lq $11, 0xB0($26)\n  lq $12, 0xC0($26)\n  lq $13, 0xD0($26)\n"
        "  lq $14, 0xE0($26)\n  lq $15, 0xF0($26)\n  lq $16, 0x100($26)\n  lq $17, 0x110($26)\n"
        "  lq $18, 0x120($26)\n  lq $19, 0x130($26)\n  lq $20, 0x140($26)\n  lq $21, 0x150($26)\n"
        "  lq $22, 0x160($26)\n  lq $23, 0x170($26)\n  lq $24, 0x180($26)\n  lq $25, 0x190($26)\n"
        "  lq $28, 0x1C0($26)\n  lq $29, 0x1D0($26)\n  lq $30, 0x1E0($26)\n  lq $31, 0x1F0($26)\n"
        "  sync.l\n"
        "  eret\n"
        "  nop\n"
        /* in the BIOS: counted, back */
        "8:\n"
        "  lui $26, %hi(p38_skipped)\n"
        "  lw $27, %lo(p38_skipped)($26)\n"
        "  addiu $27, $27, 1\n"
        "  sw $27, %lo(p38_skipped)($26)\n"
        "  lui $27, 0x8000\n"
        "  lq $29, 0x10C0($27)\n  lq $31, 0x10D0($27)\n  lq $1, 0x10E0($27)\n"
        "  sync.l\n"
        "  eret\n"
        "  nop\n"
        /* another cause: the BIOS's handler, k0 its address */
        "9:\n"
        "  lui $26, %hi(p38_orig)\n"
        "  lw $26, %lo(p38_orig)($26)\n"
        "  jr $26\n"
        "  nop\n"
        ".set pop\n");

static u32 k_access(u32 op, u32 a, u32 v) {
  if (op)
    *(volatile u32 *)a = v;
  return *(volatile u32 *)a;
}
static u32 kcall(u32 op, u32 a, u32 v) {
  register u32 a0 __asm__("$4") = op;
  register u32 a1 __asm__("$5") = a;
  register u32 a2 __asm__("$6") = v;
  register u32 v1 __asm__("$3") = 0x5A;
  register u32 v0 __asm__("$2");
  __asm__ volatile("syscall" : "=r"(v0), "+r"(v1) : "r"(a0), "r"(a1), "r"(a2) : "memory");
  return v0;
}

static void worker(void) {
  for (;;)
    count[1]++;
}

int main(void) {
  mx_bios_set_syscall(0x5A, (void *)k_access);
  mx_bios_flush_cache(0);
  p38_orig = kcall(0, INTC_ENTRY, 0);
  if ((p38_orig >> 28) != 8) {
    mx_printf("p38: INTC entry %08x: not the table expected\n", p38_orig);
    MX_TEST_END(1);
    return 1;
  }
  /* the worker's first context: its function, its stack, our gp */
  struct ctx *w = &p38_ctx[1];
  u32 gp;
  __asm__ volatile("move %0, $28" : "=r"(gp));
  w->epc = (u32)(uintptr_t)worker;
  w->r[29][0] = (u32)(uintptr_t)(worker_stack + sizeof worker_stack - 16);
  w->r[28][0] = gp;
  p38_cur = &p38_ctx[0];
  mx_bios_enable_intc(VBON);
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);

  int was = mx_di();
  kcall(1, INTC_ENTRY, 0x80000000u | ((u32)(uintptr_t)p38_entry & 0x1FFFFFFFu));
  mx_irq_restore(was);
  while (p38_vblanks < 120)
    count[0]++;
  was = mx_di();
  kcall(1, INTC_ENTRY, p38_orig);
  mx_irq_restore(was);

  mx_printf("p38: %u vblanks, %u switches, %u in the BIOS; main counted %u, the worker %u\n",
            p38_vblanks, p38_switches, p38_skipped, count[0], count[1]);
  int ok = p38_vblanks >= 120 && count[0] && count[1] && p38_switches >= 100 &&
           kcall(0, INTC_ENTRY, 0) == p38_orig;
  mx_printf("p38: %s\n", ok ? "passed" : "FAILED");
  MX_TEST_END(ok ? 0 : 1);
  return ok ? 0 : 1;
}
