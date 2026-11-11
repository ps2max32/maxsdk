/* Probe: the PS2 restarted from software, as by its reset button: the EE
   jumps to the boot ROM's reset vector (0xBFC0_0000) in kernel mode. The
   ROM (its EE path at 0xBFC0_0800: Config, Status with BEV, a TLB entry for
   the scratchpad as its stack, an initialisation at 0x9FC4_1000, the load at
   0xBFC0_0C00, the caches, the kernel at 0x8000_1000) loads the BIOS again,
   which boots as at power-on: the system update, mxboot and its one-shot.
   The IOP does not restart with it: first it is reset with the ROM's modules
   (argument "noiop": not even that). Seen on the TV and by the host link
   coming back with no program returned (ps2client status). A standalone ELF
   (libmx): started by the kernel with the BIOS's threads. */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/string.h>

/* in kernel mode (a SetSyscall handler, at EXL = 1): interrupts off, then
   the reset vector, as the CPU after a reset (the ROM sets Status itself) */
static void k_reboot(void) {
  __asm__ volatile(".set push\n.set noreorder\n"
                   "di\n"
                   "sync.p\n"
                   "lui $26, 0xbfc0\n"
                   "jr $26\n"
                   "nop\n"
                   ".set pop\n");
}

int main(int argc, char **argv) {
  int iop = !(argc >= 2 && !strcmp(argv[1], "noiop"));
  mx_printf("p40: reboot through the ROM's reset vector%s\n",
            iop ? ", the IOP reset with the ROM's modules first" : "");
  mx_debug_flush();
  if (iop && mx_iop_reset("") < 0)
    mx_printf("p40: the IOP's reset failed (going on)\n");
  mx_bios_set_syscall(0x5A, (void *)k_reboot);
  mx_dcache_flush_all();
  mx_icache_invalidate_all();
  register u32 v1 __asm__("$3") = 0x5A;
  __asm__ volatile("syscall" : "+r"(v1) : : "memory");
  mx_printf("p40: still here: the reboot did not happen\n");
  MX_TEST_END(1);
  return 1;
}
