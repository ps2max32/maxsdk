/* VM step 9: many gathers (mx_vm_pin with MX_VM_PIN_DMA) with every frame
   in use and a swap file on host:, the tables and the TLB checked after
   each (mx__vm_check: no two TLB entries for one address, frames and pages
   agree). 1 MiB of frames (32 pages), 64 pages in use, 200 pins of 1-4
   pages at random places, each followed by a check of the pinned data, a
   VIF1 transfer of it through one REF, and a few random page touches. A few
   minutes on the PS2; at the end the screen is bright green with 900D0000
   in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the round, slot 2: the pin's
   first page and length (page << 8 | pages), slot 3: mx__vm_check's last
   answer (0: consistent). Slots 4-7: swap ins, swap outs, page faults,
   pinned pages; slot 8: where the slow path is (1 entered, 2 has the
   lock, 3 done, 0 back), slot 9: the last swap operation. Heartbeat in
   slots 30 and 31.
   Slots 28 and 29 (if not blank): the VM, or a wait for the IOP, found
   interrupts off where they must be on (where << 24 | Status; see
   mx__vm_irq_lost in src/vm/vm.c).
     1 mx_vm_init with host:vm9swap.bin, 64 pages written
     2 200 rounds of pin, check, send, unpin, touch

   vm9_pins.elf long: 1000 rounds, each printing a line (console output
   through the file server right after the gather's swap writes, as when
   the file server once stopped answering in vm8_dma). */

#include "t.h"
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 64

extern volatile u32 mx__vm_where, mx__vm_swaplog[8], mx__vm_swapn;
u32 mx__vm_check(void);

static u32 gen[NP];
static u32 *pg(u32 k) { return (u32 *)((u8 *)mx_vm_base() + k * PAGE); }
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_ins);
  mx_dbg_word(5, s.swap_outs);
  mx_dbg_word(6, s.faults);
  mx_dbg_word(7, s.pinned);
  mx_dbg_word(8, mx__vm_where);
  mx_dbg_word(9, mx__vm_swaplog[(mx__vm_swapn - 1) & 7]);
}

static u32 check(void) {
  u32 c = mx__vm_check();
  mx_dbg_word(3, c);
  if (c)
    mx_printf("vm9: mx__vm_check %08x\n", c);
  return c;
}

/* a word every 64 bytes, as the pages' contents */
static void write_page(u32 k, u32 g) {
  u32 *p = pg(k);
  for (u32 i = 0; i < PAGE / 4; i += 16)
    p[i] = word(k, i, g);
}

static u32 wrong_page(u32 k, u32 g) {
  const u32 *p = pg(k);
  u32 w = 0;
  for (u32 i = 0; i < PAGE / 4; i += 16)
    w += p[i] != word(k, i, g);
  return w;
}

static u64 tags[4][2] MX_ALIGNED(64);

int main(int argc, char **argv) {
  u32 rounds = argc >= 2 && argv[1][0] == 'l' ? 1000 : 200;
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  mx_dbg_step(1);
  mx_printf("vm9: 1 mx_vm_init, %u pages written\n", NP);
  static const struct mx_vm_config cfg = {.swap = "host:vm9swap.bin", .frames = 32 * PAGE};
  int r = mx_vm_init(&cfg);
  CHECK_EQ(r, 0);
  if (r)
    return t_done("vm9");
  for (u32 k = 0; k < NP; k++) {
    mx_dbg_word(1, k);
    write_page(k, gen[k] = 1);
    stats();
  }
  CHECK_EQ(check(), 0);

  mx_dbg_step(2);
  mx_printf("vm9: 2 %u rounds\n", rounds);
  u32 x = 99, bad = 0, inconsistent = 0;
  for (u32 n = 0; n < rounds && !inconsistent; n++) {
    if (rounds > 200)
      mx_printf("vm9: round %u\n", n);
    mx_dbg_word(1, n);
    x = x * 1103515245u + 12345u;
    u32 len = 1 + (x >> 12) % 4, k = (x >> 16) % (NP - len + 1);
    mx_dbg_word(2, k << 8 | len);
    stats();
    /* from inside the first page to inside the last */
    u32 off = 64 * ((x >> 4) % 64);
    u8 *p = (u8 *)pg(k) + off;
    u32 bytes = len * PAGE - off - 64 * ((x >> 24) % 64);
    if (mx_vm_pin(p, bytes, MX_VM_PIN_DMA) != 0) {
      mx_printf("vm9: round %u: the pin failed\n", n);
      bad++;
      continue;
    }
    stats();
    inconsistent = check();
    struct mx_vm_piece pc[2];
    if (mx_vm_pieces(p, bytes, pc, 2) != 1) {
      mx_printf("vm9: round %u: not one piece\n", n);
      bad++;
    }
    for (u32 j = k; j < k + len; j++)
      if (wrong_page(j, gen[j])) {
        mx_printf("vm9: round %u: page %u lost its data\n", n, j);
        bad++;
      }
    /* the first 16 quadwords to VU1 data memory through one REF */
    mx_dcache_writeback(p, 256);
    u64 *c = &tags[0][0];
    c = mx_e_ref(c, p, 16, mx_vif_stcycl(1, 1), mx_vif_unpack(MX_UNPACK_V4_32, 0, 16, 0));
    c = mx_e_end(c, 0, 0, 0);
    mx_dcache_writeback(tags, sizeof tags);
    mx_dma_chain(MX_CH_VIF1, tags, 1);
    if (mx_dma_wait(MX_CH_VIF1) != 0)
      bad++;
    const volatile u32 *vu = (const volatile u32 *)MX_VU1_DATA, *src = (const u32 *)p;
    for (u32 i = 0; i < 64; i++)
      if (vu[i] != src[i]) {
        mx_printf("vm9: round %u: VU1 word %u wrong\n", n, i);
        bad++;
        break;
      }
    mx_vm_unpin(p, bytes);
    /* a few touches elsewhere: page faults and evictions between pins */
    for (u32 t = 0; t < 3; t++) {
      x = x * 1103515245u + 12345u;
      u32 j = (x >> 16) % NP;
      if (x >> 31)
        write_page(j, ++gen[j]);
      else if (wrong_page(j, gen[j])) {
        mx_printf("vm9: round %u: page %u wrong\n", n, j);
        bad++;
      }
    }
    if (!inconsistent)
      inconsistent = check();
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(inconsistent, 0);
  stats();
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("vm9: %u swap ins, %u outs, %u drops, %u faults, %u retries, %u pinned\n",
            s.swap_ins, s.swap_outs, s.drops, s.faults, s.retries, s.pinned);
  CHECK_EQ(s.pinned, 0);
  mx_dbg_heartbeat(0);
  return t_done("vm9");
}
