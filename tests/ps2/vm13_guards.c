/* VM step 13: in a program linked into VM, what interrupt handlers run or
   read must be core (<mx/cpu.h>, mx__need_core). Run as
     run.sh mxload.elf 30 host:vm13_guards.elf
   (PCSX2, with the EE interpreter: host:mxload.txt holding
   "host:vm13_guards.elf"). A few seconds; at the end the screen is bright
   green with 900D0000 in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the core handlers' counts,
   slot 2: the guards that refused. Heartbeat in slots 30 and 31.
     1 MX_CORE_FN handlers with MX_CORE_DATA data work: a timer and the
       vblank start, counting
     2 with mx__core_fault_hook set (instead of the report that ends the
       program), each refuses a VM function or argument and installs
       nothing: mx_timer_periodic, mx_irq_intc, mx_irq_dmac, mx_sif_serve,
       a DMA queue's on_done (the transfer runs without it) */

#include "t.h"
#include <mx/dma.h>
#include <mx/hw/formats.h>
#include <mx/irq.h>
#include <mx/loader.h>
#include <mx/sif.h>
#include <mx/timer.h>
#include <mx/vif.h>

struct counts {
  volatile u32 timer, vblank;
};
static struct counts core_counts MX_CORE_DATA;
static struct counts vm_counts; /* VM */
static struct mx_dmaq q MX_CORE_DATA;
static u32 nop_qw[4] MX_CORE_DATA MX_ALIGNED(64);

MX_CORE_FN static void core_timer(void *arg) { ((struct counts *)arg)->timer++; }
MX_CORE_FN static void core_vblank(void *arg) { ((struct counts *)arg)->vblank++; }
static void vm_handler(void *arg) { (void)arg, vm_counts.timer++; }
static void vm_server(int fno, void *arg) { (void)fno, (void)arg, vm_counts.vblank++; }

static u32 refused MX_CORE_BSS; /* the hook's (MX_CORE_FN): core */
static const char *last MX_CORE_BSS;
MX_CORE_FN static void hook(const void *p, const char *what) {
  (void)p;
  refused++;
  last = what;
}

static u32 vblanks(void) { return mx_irq_intc_count(MX_INTC_VBOF); }
static void wait_vblanks(u32 n) {
  u32 v0 = vblanks(), t0 = mx_count();
  while (vblanks() - v0 < n && mx_count() - t0 < 300000000u)
    ;
}

static void expect_refused(u32 before, const char *what) {
  mx_dbg_word(2, refused);
  mx_printf("vm13: %s: %s\n", what, refused == before + 1 ? last : "not refused");
  CHECK_EQ(refused, before + 1);
}

int main(int argc, char **argv) {
  (void)argc, (void)argv;
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  CHECK_EQ(mx_loader_mxload(), 1);

  mx_dbg_step(1);
  mx_printf("vm13: 1 core handlers\n");
  mx_timer_periodic(0, 5760, core_timer, &core_counts); /* 10 ms */
  mx_irq_intc(MX_INTC_VBON, core_vblank, &core_counts);
  wait_vblanks(20);
  mx_timer_periodic(0, 0, 0, 0);
  mx_irq_intc(MX_INTC_VBON, 0, 0);
  mx_dbg_word(1, core_counts.timer << 16 | core_counts.vblank);
  mx_printf("vm13: %u timer, %u vblank start interrupts\n", core_counts.timer, core_counts.vblank);
  CHECK(core_counts.timer >= 10);
  CHECK(core_counts.vblank >= 10);

  mx_dbg_step(2);
  mx_printf("vm13: 2 guards\n");
  mx__core_fault_hook = hook;
  u32 r = refused;
  mx_timer_periodic(0, 5760, vm_handler, 0);
  expect_refused(r, "a VM timer handler");
  r = refused;
  mx_timer_periodic(0, 5760, core_timer, &vm_counts);
  expect_refused(r, "a VM timer argument");
  r = refused;
  mx_irq_intc(MX_INTC_VBON, vm_handler, 0);
  expect_refused(r, "a VM interrupt handler");
  r = refused;
  mx_irq_intc(MX_INTC_VBON, core_vblank, &vm_counts);
  expect_refused(r, "a VM interrupt argument");
  r = refused;
  mx_irq_dmac(MX_DMAC_TOSPR, vm_handler, 0);
  expect_refused(r, "a VM DMAC handler");
  r = refused;
  CHECK(mx_sif_serve(0x7E570000u, vm_server, 0) < 0);
  expect_refused(r, "a VM SIF server");
  wait_vblanks(10);
  CHECK_EQ(vm_counts.timer, 0);
  CHECK_EQ(vm_counts.vblank, 0);

  r = refused;
  mx_dmaq_init(&q, MX_CH_VIF1);
  q.on_done = vm_handler;
  nop_qw[0] = nop_qw[1] = nop_qw[2] = nop_qw[3] = mx_vif_nop();
  mx_dcache_writeback(nop_qw, sizeof nop_qw);
  mx_event e = mx_dmaq_send(&q, nop_qw, 1);
  expect_refused(r, "a VM on_done");
  mx_event_wait(e);
  mx_dmaq_release(&q);
  CHECK_EQ(vm_counts.timer, 0);
  mx__core_fault_hook = 0;

  mx_dbg_heartbeat(0);
  return t_done("vm13");
}
