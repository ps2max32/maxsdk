/* The units at a program's end (kernel version 45, apps/mxos ee/end.c): a
   program that ends with VU1 running leaves the next one VU1 stopped and
   VIF1 idle. Two runs, one after the other:
     k84_vuend.elf loop   VU1 started in an endless loop (B to itself), its
                          VPU_STAT VBS1 checked, then the end (status 0)
     k84_vuend.elf dma    a VIF1 DMA chain that calls itself for ever, each
                          time an MSCAL of a short VU1 program: VU1 started
                          again and again, then the end (status 0); with the
                          channel stopped by STR = 0 alone, VU1 went on
     k84_vuend.elf        VBS0 and VBS1 clear, VIF1 idle, and a VIF1 FLUSH
                          through its FIFO done at once (it waits for VU1:
                          with VU1 still running, it waited for good)
   (status 0 expected from each) */
#include "t.h"
#include <mx/hw/regs.h>
#include <mx/kernel.h>
#include <mx/string.h>
#include <mx/cpu.h>
#include <mx/vu.h>

#define VU1_MICRO 0x11008000u /* VU1's micro memory, mapped while VU1 stops */

static void vu1_loop(void) {
  volatile u32 *m = (volatile u32 *)VU1_MICRO;
  /* 0: B to 0 (Imm11 -1: the delay slot's address + -1 * 8) | NOP;
     8: the delay slot, NOP | NOP */
  m[0] = 0x40000000u | 0x7FFu, m[1] = 0x000002FFu;
  m[2] = 0x8000033Cu, m[3] = 0x000002FFu;
  __asm__ volatile("sync.l");
  /* ctc2 $8, vi31 (CMSAR1): VU1 starts at 0 */
  __asm__ volatile("move $8, $0\n\t.word 0x48c8f800\n\tsync.p" : : : "$8", "memory");
}

/* a tag NEXT to itself (QWC 1), then its qword: MSCAL 0 and three NOPs */
static u32 chain[8] MX_IRQ_DATA MX_ALIGNED(64);

static void vif1_forever(void) {
  volatile u32 *m = (volatile u32 *)VU1_MICRO;
  /* 0: NOP | NOP[E]; 8: NOP | NOP (the E bit's delay slot) */
  m[0] = 0x8000033Cu, m[1] = 0x400002FFu;
  m[2] = 0x8000033Cu, m[3] = 0x000002FFu;
  u32 a = mx_paddr_of(chain).v;
  volatile u32 *c = MX_UNCACHED(chain);
  c[0] = 1u | 2u << 28, c[1] = a, c[2] = 0, c[3] = 0; /* QWC 1, ID NEXT (2), ADDR itself */
  c[4] = 0x14000000u, c[5] = 0, c[6] = 0, c[7] = 0;    /* MSCAL 0, NOP, NOP, NOP */
  __asm__ volatile("sync.l");
  MX_R32(0x10009030u) = a; /* D1_TADR */
  MX_R32(0x10009020u) = 0; /* D1_QWC */
  MX_R32(MX_DMA_VIF1) = 0x105u; /* DIR from memory, chain mode, STR */
}

int main(int argc, char **argv) {
  CHECK(mx__kernel && mx__kernel->version >= 45);
  u32 st = mx_vpu_stat();
  if (argc > 1 && !strcmp(argv[1], "loop")) {
    CHECK_EQ(st & MX_VPU_VBS1, 0); /* not running yet */
    vu1_loop();
    for (int i = 0; i < 1000 && !(mx_vpu_stat() & MX_VPU_VBS1); i++)
      ;
    st = mx_vpu_stat();
    mx_printf("k84: VU1 started in a loop: VPU_STAT %08x; the end now\n", st);
    CHECK(st & MX_VPU_VBS1);
    return t_done("k84");
  }
  if (argc > 1 && !strcmp(argv[1], "dma")) {
    vif1_forever();
    u32 seen = 0;
    for (int i = 0; i < 100000; i++)
      seen |= mx_vpu_stat();
    mx_printf("k84: VIF1's chain runs: VPU_STAT seen %08x, D1_CHCR %08x; the end now\n", seen,
              MX_R32(MX_DMA_VIF1));
    CHECK(seen & MX_VPU_VBS1);
    return t_done("k84");
  }
  mx_printf("k84: at the start VPU_STAT %08x, VIF1_STAT %08x\n", st, MX_R32(MX_VIF_VIF1_STAT));
  CHECK_EQ(st & (MX_VPU_VBS0 | MX_VPU_VBS1), 0);
  /* a FLUSH (VIFcode 0x11) and a NOP into VIF1's FIFO: done when VU1 is not
     running (VIF1_STAT's VEW, bit 2: waits for the end of a micro program) */
  volatile u32 *fifo = (volatile u32 *)MX_VIF1_FIFO;
  fifo[0] = 0x11000000u, fifo[1] = 0, fifo[2] = 0, fifo[3] = 0;
  u32 vew = 0;
  for (int i = 0; i < 1000; i++)
    vew = MX_R32(MX_VIF_VIF1_STAT) & 4u;
  mx_printf("k84: after a FLUSH VIF1_STAT %08x\n", MX_R32(MX_VIF_VIF1_STAT));
  CHECK_EQ(vew, 0);
  return t_done("k84");
}
