/* The VM's writer thread (mx_vm_config.clean_frames) for an MXOS program,
   also one that gets a space because another program's VM is on (launched
   by a program with VM: the shell's games). 16 frames, clean_frames 4, 48
   pages: 300 random touches (a page written whole, or checked) with 5 ms
   of sleep after each, as a game's frame, in which the writer gets ahead.
   Most write outs must be the writer's.
     vm45_writer.elf                                    (the VM its own)
     vm26_launch.elf launch - host:vm45_writer.elf      (a space; status 0) */

#include "t.h"
#include <mx/thread.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48

const struct mx_vm_config mx_vm_config = {
    .swap = "host:vm45swap.bin", .frames = 16 * PAGE, .clean_frames = 4};

static u32 pages[NP][PAGE / 4];
static u32 gen[NP];
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }

int main(void) {
  CHECK(mx_vm_active());
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE / 4; i++)
      pages[k][i] = word(k, i, 0);
  u32 seed = 12345, bad = 0;
  for (u32 n = 0; n < 300; n++) {
    seed = seed * 1664525u + 1013904223u;
    u32 k = (seed >> 8) % NP;
    if (seed >> 30 & 1) {
      gen[k]++;
      for (u32 i = 0; i < PAGE / 4; i++)
        pages[k][i] = word(k, i, gen[k]);
    } else
      for (u32 i = 0; i < PAGE / 4; i++)
        bad += pages[k][i] != word(k, i, gen[k]);
    mx_sleep_us(5000);
  }
  for (u32 k = 0; k < NP; k++)
    for (u32 i = 0; i < PAGE / 4; i++)
      bad += pages[k][i] != word(k, i, gen[k]);
  struct mx_vm_stats st;
  mx_vm_stats(&st);
  mx_printf("vm45: %u words wrong; %u swap outs by faults, %u by the writer, %u ins\n", bad,
            st.swap_outs, st.writer_outs, st.swap_ins);
  CHECK_EQ(bad, 0);
  CHECK(st.writer_outs > st.swap_outs);
  return t_done("vm45");
}
