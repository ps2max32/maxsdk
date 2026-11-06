/* VM step 7: paging through a swap file on host: (<mx/vm.h>). 1 MiB of
   frames (32 pages) for up to 96 pages in use, so pages go out to the
   file and come back in the faulting thread. About a minute on the PS2;
   at the end the screen is bright green with 900D0000 in slot 0, or bright
   red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the page being touched, slot
   2: the round (step 5). Slots 4-7: swap ins, swap outs, page faults,
   retries; slot 8: where the slow path is (1 entered, 2 has the lock, 3
   done, 0 back), slot 9: the last swap operation (op << 31 | slot << 16 |
   frame << 4 | 1 sought, 2 done). Heartbeat in slots 30 and 31.
   Slots 28 and 29 (if not blank): the VM, or a wait for the IOP, found
   interrupts off where they must be on (where << 24 | Status; see
   mx__vm_irq_lost in ee/src/vm/vm.c).
     1 mx_vm_init with host:vm7swap.bin
     2 a word in each of 64 pages, then read back (half of them from the
       file)
     3 64 pages written and checked whole
     4 300 random touches of 96 pages (a write, or a check of the page)
     5 a second thread checking 32 pages while the main thread writes 32
       others, both faulting */

#include "t.h"
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define FRAMES 32

extern volatile u32 mx__vm_where, mx__vm_swaplog[8], mx__vm_swapn;

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_ins);
  mx_dbg_word(5, s.swap_outs);
  mx_dbg_word(6, s.faults);
  mx_dbg_word(7, s.retries);
  mx_dbg_word(8, mx__vm_where);
  mx_dbg_word(9, mx__vm_swaplog[(mx__vm_swapn - 1) & 7]);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm7: %u %s\n", n, what);
}

static u32 *pg(u32 k) { return (u32 *)((u8 *)mx_vm_base() + k * PAGE); }
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }

static void write_page(u32 k, u32 g) {
  mx_dbg_word(1, k);
  u32 *p = pg(k);
  for (u32 i = 0; i < PAGE / 4; i++)
    p[i] = word(k, i, g);
  stats();
}

static u32 check_page(u32 k, u32 g) {
  mx_dbg_word(1, k);
  const u32 *p = pg(k);
  u32 w = 0;
  for (u32 i = 0; i < PAGE / 4; i++)
    w += p[i] != word(k, i, g);
  stats();
  return w;
}

static u32 gen[96];

static u8 tstack[16384] MX_ALIGNED(16);
static volatile u32 other_bad, other_done;
static void other(void *arg) {
  (void)arg;
  for (u32 k = 0; k < 32; k++)
    other_bad += check_page(k, gen[k]) != 0;
  other_done = 1;
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "mx_vm_init");
  static const struct mx_vm_config cfg = {.swap = "host:vm7swap.bin", .frames = FRAMES * PAGE};
  int r = mx_vm_init(&cfg);
  mx_dbg_word(1, (u32)r);
  CHECK_EQ(r, 0);
  if (r)
    return t_done("vm7");

  step(2, "a word in each of 64 pages");
  for (u32 k = 0; k < 64; k++) {
    mx_dbg_word(1, k);
    pg(k)[k] = word(k, k, 1);
    stats();
  }
  u32 bad = 0;
  for (u32 k = 0; k < 64; k++) {
    mx_dbg_word(1, k);
    bad += pg(k)[k] != word(k, k, 1);
    stats();
  }
  CHECK_EQ(bad, 0);
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("vm7: %u swap ins, %u outs, %u faults, %u retries\n", s.swap_ins, s.swap_outs,
            s.faults, s.retries);
  CHECK(s.swap_outs >= 32);
  CHECK(s.swap_ins >= 32);

  step(3, "64 whole pages");
  for (u32 k = 0; k < 64; k++)
    write_page(k, gen[k] = 2);
  bad = 0;
  for (u32 k = 0; k < 64; k++)
    bad += check_page(k, 2) != 0;
  CHECK_EQ(bad, 0);

  step(4, "300 random touches");
  for (u32 k = 64; k < 96; k++)
    write_page(k, gen[k] = 2);
  u32 x = 7;
  bad = 0;
  for (u32 n = 0; n < 300; n++) {
    mx_dbg_word(2, n);
    x = x * 1103515245u + 12345u;
    u32 k = (x >> 16) % 96;
    if (x >> 30 & 1)
      write_page(k, ++gen[k]);
    else if (check_page(k, gen[k])) {
      bad++;
      mx_printf("vm7: page %u wrong at round %u\n", k, n);
    }
  }
  CHECK_EQ(bad, 0);

  step(5, "a second thread");
  mx_thread_set_prio(mx_thread_self(), 64);
  int t = mx_thread_start(other, 0, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  for (u32 k = 32; k < 64; k++) {
    write_page(k, ++gen[k]);
    mx_thread_yield();
  }
  mx_thread_join(t);
  CHECK_EQ(other_done, 1);
  CHECK_EQ(other_bad, 0);
  for (u32 k = 32; k < 64; k++)
    CHECK_EQ(check_page(k, gen[k]), 0);

  stats();
  mx_vm_stats(&s);
  mx_printf("vm7: %u swap ins, %u outs, %u drops, %u faults, %u retries, %u refills\n",
            s.swap_ins, s.swap_outs, s.drops, s.faults, s.retries, s.refills);
  mx_printf("vm7: %u file server calls (%u.%02u a transfer); ms in swap ins %u, outs %u, "
            "the VM lock %u\n",
            s.io_calls, s.io_calls / (s.swap_ins + s.swap_outs),
            s.io_calls * 100 / (s.swap_ins + s.swap_outs) % 100, s.us_swap_in / 1000,
            s.us_swap_out / 1000, s.us_lock / 1000);
  mx_dbg_heartbeat(0);
  return t_done("vm7");
}
