/* The one swap file of MXOS programs (mx OS plan 3c): with
   mc0:/MXOS/SWAP.CFG (on PCSX2 host:swap.txt: a path and a size in MiB) the
   kernel gives this program that file in place of the one it asks for
   (host:vm39own.bin), and the VM uses at most the size. 16 frames, 48
   pages written and checked: with 4 MiB (128 slots) it passes, with 1 MiB
   (32 slots) the VM stops with "the swap file is full".
     vm39_swapcfg.elf     (status 0 expected with 4 MiB) */

#include "t.h"
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48

const struct mx_vm_config mx_vm_config = {.swap = "host:vm39own.bin", .frames = 16 * PAGE};

static u8 pages[NP][PAGE];

int main(void) {
  CHECK(mx_vm_active());
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      pages[k][i] = (u8)(k * 7 + i / 64);
  u32 bad = 0;
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE; i += 64)
      bad += pages[k][i] != (u8)(k * 7 + i / 64);
  struct mx_vm_stats st;
  mx_vm_stats(&st);
  mx_printf("vm39: %u pages, %u bytes wrong; %u swap outs, %u ins\n", NP, bad, st.swap_outs,
            st.swap_ins);
  CHECK_EQ(bad, 0);
  CHECK(st.swap_outs > 0);
  return t_done("vm39");
}
