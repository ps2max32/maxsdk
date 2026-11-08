/* A DMAC channel's end as an interrupt under mx-kernel's own dispatch
   (version 10, docs/mx-os-plan.md step 2d): mx_irq_dmac on the GIF channel
   turns its CIM bit on, a GIF transfer's end runs the handler once. D_STAT
   is shown before, after mx_irq_dmac and after the transfer. Linked into VM,
   started by mxlink. */
#include "t.h"
#include <mx/dma.h>
#include <mx/hw/regs.h>
#include <mx/irq.h>

static volatile u32 got;
MX_CORE_FN static void handler(void *arg) {
  (void)arg;
  got++;
}

static u64 pkt[4] MX_DMA_LINES;

int main(void) {
  mx_printf("vm30: D_STAT %08x\n", MX_R32(MX_D_STAT));
  /* an earlier transfer's end (the kernel's screen) cleared first, as
     mx_dmaq_init does: else it comes at once */
  MX_R32(MX_D_STAT) = 1u << MX_DMAC_GIF;
  mx_irq_dmac(MX_DMAC_GIF, handler, 0);
  u32 st = MX_R32(MX_D_STAT);
  mx_printf("vm30: after mx_irq_dmac %08x\n", st);
  CHECK(st >> (16 + MX_DMAC_GIF) & 1);
  /* one GIF tag, PACKED, A+D, a NOP */
  pkt[0] = 1ull | 1ull << 15 | 1ull << 60;
  pkt[1] = 0xE;
  pkt[2] = 0;
  pkt[3] = 0x7F;
  mx_dcache_writeback(pkt, sizeof pkt);
  mx_dma_send(MX_CH_GIF, pkt, 2);
  mx_dma_wait(MX_CH_GIF);
  for (u32 t0 = mx_count(); !got && mx_count() - t0 < 294912000u;)
    ;
  mx_printf("vm30: handler ran %u times, D_STAT %08x\n", got, MX_R32(MX_D_STAT));
  CHECK_EQ(got, 1);
  mx_irq_dmac(MX_DMAC_GIF, 0, 0);
  return t_done("vm30");
}
