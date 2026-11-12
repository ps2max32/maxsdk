/* Probe (mx OS plan, step 2d-1): the kernel first at every interrupt.

   The BIOS's interrupt vector (0x8000_0200) saves sp, ra and at at
   0x8000_10C0, takes the highest pending line of Cause.IP & Status.IM and
   jumps (jr k0) to a handler from the table at 0x8001_5380, indexed by 30 -
   PLZCW of those bits (leading zeros - 1): 2 for INT0 (the INTC), 3 for INT1
   (the DMAC), 7 for the COP0 timer. Here those two entries point at stubs
   of this probe (through kseg0, so no TLB or ASID is involved) that count
   and jump on to the BIOS's handler with k0 as the vector left it; only k1
   is changed. The table is written by a syscall handler (kernel mode).

   Measured: the stubs see every interrupt (vblank starts against
   mx_gs.vsync), and what a stub costs (a loop's rate with and without). The
   entries are set back at the end. Run by mxlink (a program on libmx). */

#include <mx/bios.h>
#include <mx/mx.h>

#define TABLE 0x80015380u
#define INTC_ENTRY (TABLE + 8)  /* 0x8000_0380 */
#define DMAC_ENTRY (TABLE + 12) /* 0x8000_04C0 */

/* k0: the BIOS's handler's address on entry (the vector's jr k0); the stub
   counts, then jumps to the saved original with k0 = its address, as the
   vector would. */
volatile u32 p37_intc_count, p37_dmac_count;
u32 p37_intc_orig, p37_dmac_orig;
void p37_intc_stub(void);
void p37_dmac_stub(void);
__asm__(".set push\n.set noreorder\n.set noat\n"
        ".globl p37_intc_stub\np37_intc_stub:\n"
        "  lui $27, %hi(p37_intc_count)\n"
        "  lw $26, %lo(p37_intc_count)($27)\n"
        "  addiu $26, $26, 1\n"
        "  sw $26, %lo(p37_intc_count)($27)\n"
        "  lui $26, %hi(p37_intc_orig)\n"
        "  lw $26, %lo(p37_intc_orig)($26)\n"
        "  jr $26\n"
        "  nop\n"
        ".globl p37_dmac_stub\np37_dmac_stub:\n"
        "  lui $27, %hi(p37_dmac_count)\n"
        "  lw $26, %lo(p37_dmac_count)($27)\n"
        "  addiu $26, $26, 1\n"
        "  sw $26, %lo(p37_dmac_count)($27)\n"
        "  lui $26, %hi(p37_dmac_orig)\n"
        "  lw $26, %lo(p37_dmac_orig)($26)\n"
        "  jr $26\n"
        "  nop\n"
        ".set pop\n");

/* Kernel memory read or written by a syscall handler (kernel mode):
   SetSyscall(0x5A) as probe/kdump.c. op 0: read a, 1: write v to a. */
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

static u32 kseg0(void (*f)(void)) { return 0x80000000u | ((u32)(uintptr_t)f & 0x1FFFFFFFu); }

/* a loop's iterations in n vblanks */
static u32 rate(u32 n) {
  u32 v = mx_gs.vsync;
  while (mx_gs.vsync == v)
    ;
  v = mx_gs.vsync;
  u32 i = 0;
  while (mx_gs.vsync - v < n)
    i++;
  return i;
}

int main(void) {
  mx_gs_init(640, 448);
  mx_bios_set_syscall(0x5A, (void *)k_access);
  mx_bios_flush_cache(0);
  p37_intc_orig = kcall(0, INTC_ENTRY, 0);
  p37_dmac_orig = kcall(0, DMAC_ENTRY, 0);
  mx_printf("p37: table %08x: INTC %08x, DMAC %08x\n", TABLE, p37_intc_orig, p37_dmac_orig);
  if ((p37_intc_orig >> 28) != 8 || (p37_dmac_orig >> 28) != 8) {
    mx_printf("p37: not the table expected\n");
    MX_TEST_END(1);
    return 1;
  }

  u32 base = rate(120);
  mx_bios_flush_cache(0);
  mx_bios_flush_cache(2);
  int was = mx_di();
  kcall(1, INTC_ENTRY, kseg0(p37_intc_stub));
  kcall(1, DMAC_ENTRY, kseg0(p37_dmac_stub));
  mx_irq_restore(was);
  u32 v0 = mx_gs.vsync, c0 = p37_intc_count;
  u32 hooked = rate(120);
  u32 vb = mx_gs.vsync - v0, ic = p37_intc_count - c0, dc = p37_dmac_count;
  was = mx_di();
  kcall(1, INTC_ENTRY, p37_intc_orig);
  kcall(1, DMAC_ENTRY, p37_dmac_orig);
  mx_irq_restore(was);
  u32 after = rate(120);

  mx_printf("p37: %u vblanks, the INTC stub %u times, the DMAC stub %u times\n", vb, ic, dc);
  mx_printf("p37: loop rate %u without, %u with the stubs, %u after (%d.%03d%%)\n", base, hooked,
            after, (int)((s32)(base - hooked) * 100 / (s32)base),
            (int)(((s32)(base - hooked) * 100000 / (s32)base) % 1000));
  u32 back1 = kcall(0, INTC_ENTRY, 0), back2 = kcall(0, DMAC_ENTRY, 0);
  int ok = ic >= vb && back1 == p37_intc_orig && back2 == p37_dmac_orig;
  mx_printf("p37: %s\n", ok ? "passed" : "FAILED");
  MX_TEST_END(ok ? 0 : 1);
  return ok ? 0 : 1;
}
