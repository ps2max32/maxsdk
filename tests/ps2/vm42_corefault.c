/* What an interrupt handler reads (its argument), or what the DMAC reads,
   in VM that is not pinned: the program ends with a report and status
   255, and the kernel goes on (mx OS plan 4b; before, the kernel's own
   mx_exit ran and the PS2 hung). Started by mxlink:
     vm42_corefault.elf arg     the handler's argument in VM
     vm42_corefault.elf dma     mx_paddr_of of VM
   (status 255 expected, the kernel ready after it) */

#include "t.h"
#include <mx/dma.h>
#include <mx/irq.h>
#include <mx/string.h>

static u32 in_vm[64]; /* .bss of the program: VM */

MX_CORE_FN static void handler(void *arg) { (void)arg; }

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "arg")) {
    mx_printf("vm42: a handler with its argument in VM\n");
    mx_irq_dmac(MX_DMAC_GIF, handler, in_vm);
  } else {
    mx_printf("vm42: mx_paddr_of of VM\n");
    mx_paddr a = mx_paddr_of(in_vm);
    mx_printf("vm42: %08x\n", a.v);
  }
  mx_printf("vm42: not ended\n");
  return t_done("vm42");
}
