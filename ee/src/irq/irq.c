#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/hw/formats.h>
#include <mx/irq.h>
#include <mx/trace.h>

struct slot {
  mx_irq_fn fn;
  void *arg;
  int bios_id; /* 0 = not registered with the BIOS yet */
  volatile u32 count;
};

static struct slot dmac[10];
static struct slot intc[15];

static int dmac_entry(int ch, void *a, void *addr) {
  (void)a, (void)addr;
  struct slot *s = &dmac[ch];
  mx_trace(MX_TR_DMAC_IRQ, (u16)ch);
  s->count++;
  if (s->fn)
    s->fn(s->arg);
  return 0;
}

/* the interrupted code's address of the INTC interrupt being handled (the
   BIOS's third argument), for mx_prof */
volatile u32 mx__irq_epc;

/* the SDK's own work at each vblank start, next to the slot's handler
   (the SIF waits' timeouts, ee/src/base/sif.c; the VM trace's sweeps,
   ee/src/vm/vm.c) */
static void (*vbon_hook[2])(void);

static int intc_entry(int cause, void *a, void *addr) {
  (void)a;
  mx__irq_epc = (u32)(uintptr_t)addr;
  struct slot *s = &intc[cause];
  mx_trace(MX_TR_INTC_IRQ, (u16)cause);
  s->count++;
  if (s->fn)
    s->fn(s->arg);
  if (cause == MX_INTC_VBON)
    for (int i = 0; i < 2; i++)
      if (vbon_hook[i])
        vbon_hook[i]();
  return 0;
}

/* One more function at each vblank start (two at most). */
void mx__irq_vbon_hook(void (*fn)(void)) {
  struct slot *s = &intc[MX_INTC_VBON];
  int was = mx_di();
  for (int i = 0; i < 2; i++)
    if (!vbon_hook[i] || vbon_hook[i] == fn) {
      vbon_hook[i] = fn;
      break;
    }
  mx_irq_restore(was);
  if (!s->bios_id)
    s->bios_id = mx_bios_add_intc_handler(MX_INTC_VBON, intc_entry, 0, 0);
  mx_bios_enable_intc(MX_INTC_VBON);
}

void mx_irq_dmac(int ch, mx_irq_fn fn, void *arg) {
  if (mx__need_core((const void *)fn, "mx_irq_dmac: the handler") ||
      mx__need_core(arg, "mx_irq_dmac: the handler's argument"))
    return;
  struct slot *s = &dmac[ch];
  int was = mx_di();
  s->fn = fn;
  s->arg = arg;
  mx_irq_restore(was);
  if (fn && !s->bios_id)
    s->bios_id = mx_bios_add_dmac_handler(ch, dmac_entry, 0, 0);
  if (fn)
    mx_bios_enable_dmac(ch);
  else
    mx_bios_disable_dmac(ch);
}

void mx_irq_intc(int cause, mx_irq_fn fn, void *arg) {
  if (mx__need_core((const void *)fn, "mx_irq_intc: the handler") ||
      mx__need_core(arg, "mx_irq_intc: the handler's argument"))
    return;
  struct slot *s = &intc[cause];
  int was = mx_di();
  s->fn = fn;
  s->arg = arg;
  mx_irq_restore(was);
  if (fn && !s->bios_id)
    s->bios_id = mx_bios_add_intc_handler(cause, intc_entry, 0, 0);
  if (fn)
    mx_bios_enable_intc(cause);
  else if (!(cause == MX_INTC_VBON && vbon_hook[0]))
    mx_bios_disable_intc(cause);
}

void mx_irq_exit(void) {
  vbon_hook[0] = vbon_hook[1] = 0;
  for (int i = 0; i < 10; i++)
    if (dmac[i].bios_id) {
      mx_bios_disable_dmac(i);
      mx_bios_remove_dmac_handler(i, dmac[i].bios_id);
      dmac[i] = (struct slot){0};
    }
  for (int i = 0; i < 15; i++)
    if (intc[i].bios_id) {
      mx_bios_disable_intc(i);
      mx_bios_remove_intc_handler(i, intc[i].bios_id);
      intc[i] = (struct slot){0};
    }
}

u32 mx_irq_dmac_count(int ch) { return dmac[ch].count; }
u32 mx_irq_intc_count(int cause) { return intc[cause].count; }
