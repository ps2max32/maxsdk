/* VM step 6: VM on without a swap file (<mx/vm.h>): TLB refills, new pages
   zero-filled in the thread (the exception at EXL = 1 only refills the
   TLB, marks pages dirty and maps resident ones again), dirty tracking.
   4 MiB of frames, so 128 pages; the TLB has 17 entries for them. About
   10 s; at the end the screen is bright green with 900D0000 in slot 0, or
   bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the page being touched, slots
   4-7: refills, page faults (in the thread), zero fills, free frames.
   Heartbeat in slots 30 and 31.
     1 mx_vm_init
     2 a load from page 0: zero, one fault
     3 a word stored in each of the 128 pages, then read back
     4 every page written and checked whole, twice (refills all the time)
     5 a second thread checking pages while the main thread writes others
     6 a timer interrupt reading a pinned (so resident) page
     7 reads through each of the BIOS's 8 UCAB entries (31-38, which VM
       pages have replaced: put back on a miss), the odd page of each too;
       then the tables and the TLB consistent (slot 3: mx__vm_check) */

#include "t.h"
#include <mx/vm.h>

#define PAGE MX_VM_PAGE

u32 mx__vm_check(void);
#define NP 128

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.refills);
  mx_dbg_word(5, s.faults);
  mx_dbg_word(6, s.zero_fills);
  mx_dbg_word(7, s.free_frames);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm6: %u %s\n", n, what);
}

static u32 *pg(u32 k) { return (u32 *)((u8 *)mx_vm_base() + k * PAGE); }
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }

static void write_page(u32 k, u32 g) {
  u32 *p = pg(k);
  for (u32 i = 0; i < PAGE / 4; i++)
    p[i] = word(k, i, g);
}

static u32 check_page(u32 k, u32 g) {
  const u32 *p = pg(k);
  u32 w = 0;
  for (u32 i = 0; i < PAGE / 4; i++)
    w += p[i] != word(k, i, g);
  return w;
}

static u8 tstack[16384] MX_ALIGNED(16);
static volatile u32 other_bad, other_done;
static void other(void *arg) {
  (void)arg;
  for (u32 r = 0; r < 4; r++)
    for (u32 k = 0; k < NP / 2; k++)
      other_bad += check_page(k, 2) != 0;
  other_done = 1;
}

static volatile u32 ticks, tick_bad;
static void tick(void *arg) {
  (void)arg;
  ticks++;
  tick_bad += pg(NP - 1)[5] != word(NP - 1, 5, 2);
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "mx_vm_init");
  static const struct mx_vm_config cfg = {.frames = NP * PAGE};
  int r = mx_vm_init(&cfg);
  mx_dbg_word(1, (u32)r);
  CHECK_EQ(r, 0);
  if (r)
    return t_done("vm6");
  CHECK_EQ(mx_vm_size(), NP * PAGE);

  step(2, "a load from page 0");
  CHECK_EQ(pg(0)[0], 0);
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  CHECK_EQ(s.faults, 1);
  CHECK_EQ(s.zero_fills, 1);
  mx_printf("vm6: dirty tracking %u\n", s.dirty_tracking);

  step(3, "a word in each page");
  for (u32 k = 0; k < NP; k++) {
    mx_dbg_word(1, k);
    pg(k)[k] = word(k, k, 1);
  }
  u32 bad = 0;
  for (u32 k = 0; k < NP; k++)
    bad += pg(k)[k] != word(k, k, 1) || pg(k)[(k + 1) % 64] != 0;
  CHECK_EQ(bad, 0);
  mx_vm_stats(&s);
  CHECK_EQ(s.zero_fills, NP);
  CHECK_EQ(s.free_frames, 0);

  step(4, "whole pages, twice");
  for (u32 g = 2; g < 4; g++) {
    for (u32 k = 0; k < NP; k++) {
      mx_dbg_word(1, k);
      write_page(k, g);
    }
    bad = 0;
    for (u32 k = 0; k < NP; k++)
      bad += check_page(k, g) != 0;
    CHECK_EQ(bad, 0);
    stats();
  }
  for (u32 k = 0; k < NP; k++)
    write_page(k, 2);

  step(5, "a second thread");
  mx_thread_set_prio(mx_thread_self(), 64);
  int t = mx_thread_start(other, 0, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  for (u32 r2 = 0; r2 < 4; r2++) {
    for (u32 k = NP / 2; k < NP; k++) {
      mx_dbg_word(1, k);
      write_page(k, 2);
      if (!(k & 7))
        mx_thread_yield();
    }
  }
  mx_thread_join(t);
  CHECK_EQ(other_done, 1);
  CHECK_EQ(other_bad, 0);

  step(6, "a timer reading a pinned page");
  CHECK_EQ(mx_vm_pin(pg(NP - 1), PAGE, 0), 0);
  mx_timer_periodic(1, 57600, tick, 0);
  u32 v0 = mx_gs.vsync;
  while (mx_gs.vsync - v0 < 2 * mx_gs.hz)
    for (u32 k = 0; k < NP - 1; k++)
      bad += check_page(k, 2) != 0;
  mx_timer_periodic(1, 0, 0, 0);
  mx_vm_unpin(pg(NP - 1), PAGE);
  CHECK_EQ(bad, 0);
  CHECK(ticks > 5);
  CHECK_EQ(tick_bad, 0);

  step(7, "UCAB");
  /* the UCAB entries' ranges (0x3010_0000 on: 2 x 256 KiB, 3 x 1 MiB, 3 x
     4 MiB pairs), a word near the end of each half */
  static const u32 at[8] = {0x30100000, 0x30180000, 0x30200000, 0x30400000,
                            0x30600000, 0x30800000, 0x31000000, 0x31800000};
  static const u32 pair[8] = {0x80000, 0x80000, 0x200000, 0x200000,
                              0x200000, 0x800000, 0x800000, 0x800000};
  for (u32 r2 = 0; r2 < 2; r2++)
    for (u32 i = 0; i < 8; i++)
      for (u32 h = 1; h <= 2; h++) {
        u32 a = at[i] + h * (pair[i] / 2) - 64;
        /* only read (any memory: the program, frames): the same word
           through the cache, written back first, and through UCAB */
        const volatile u32 *c = MX_CACHED((const volatile u32 *)(uintptr_t)a);
        mx_dcache_writeback((const void *)c, 4);
        mx_dbg_word(1, a);
        CHECK_EQ(*(volatile u32 *)(uintptr_t)a, *c);
        for (u32 k = 0; k < 20; k++) /* VM pages take the TLB entries again */
          (void)pg(k)[0];
      }
  u32 c = mx__vm_check();
  mx_dbg_word(3, c);
  CHECK_EQ(c, 0);

  stats();
  mx_vm_stats(&s);
  mx_printf("vm6: %u refills, %u faults, %u zero fills, %u ticks\n", s.refills, s.faults,
            s.zero_fills, ticks);
  mx_dbg_heartbeat(0);
  return t_done("vm6");
}
