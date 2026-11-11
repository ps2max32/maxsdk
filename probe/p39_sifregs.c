/* Probe (mx OS plan, step 2g): the SIF's registers read by a program
   itself, against the BIOS's SifGetReg.

   MSCOM (0x1000_F200), SMCOM (F210), MSFLG (F220) and SMFLG (F230) read at
   their kuseg addresses (the BIOS's TLB maps the page for programs), beside
   SifGetReg(1..4), with the CPU's mode (Status.KSU, EXL, ERL: a program runs
   in user mode, KSU 2, where kseg1 is an address error). The EE manual
   lists only SB_SMFLG of them among the registers a program uses.
   Measured (SCPH-70004, 2026-11-11): user mode reads all four, the same
   values as SifGetReg. Run by the kernel (a program on libmx). */

#include <mx/bios.h>
#include <mx/cpu.h>
#include <mx/mx.h>

#define R32(a) (*(volatile u32 *)(uintptr_t)(a))

int main(void) {
  u32 st = mx_status();
  mx_printf("p39: status %08x: KSU %u EXL %u ERL %u\n", st, (st >> 3) & 3, (st >> 1) & 1, (st >> 2) & 1);
  int bad = 0;
  for (u32 reg = 1; reg <= 4; reg++) {
    u32 off = 0xF200 + (reg - 1) * 0x10;
    u32 bios = (u32)mx_bios_sif_get_reg(reg);
    u32 user = R32(0x10000000u + off);
    mx_printf("p39: reg %u (%04x): BIOS %08x, kuseg %08x%s\n", reg, off, bios, user,
              user != bios ? ": kuseg differs" : "");
    bad |= user != bios;
  }
  mx_printf("p39: %s\n", bad ? "a register reads differently at kuseg" : "all the same");
  MX_TEST_END(bad);
  return bad;
}
