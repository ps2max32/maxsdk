/* VM step 17: the writer thread (mx_vm_config.clean_frames): dirty pages
   are written to the swap file ahead, so page faults find clean victims
   and only read. 16 frames (512 KiB), a swap file, clean_frames 4. Under a
   minute; at the end the screen is bright green with 900D0000 in slot 0,
   or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the round, slot 2: the main
   thread's priority. Slots 4-7: swap outs by faults, by the writer, swap
   ins, mx__vm_check's last answer (0: consistent). Heartbeat in slots 30
   and 31.
     1 mx_vm_init with clean_frames 4 (the main thread at priority 64, the
       writer above it)
     2 48 pages written whole, checked
     3 400 random touches of 48 pages (a write of the whole page, or a
       check), 10 ms of work after each (a game's frame: the writer gets
       ahead meanwhile), the tables checked every 50
     4 a second thread checking 16 pages while the main thread writes 16
       others; then everything checked again */

#include "t.h"
#include <mx/thread.h>
#include <mx/timer.h>
#include <mx/mem.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 48

u32 mx__vm_check(void);

static u32 gen[NP];
static u32 *pg(u32 k) { return (u32 *)((u8 *)mx_vm_base() + k * PAGE); }
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_outs);
  mx_dbg_word(5, s.writer_outs);
  mx_dbg_word(6, s.swap_ins);
}

static void write_page(u32 k, u32 g) {
  u32 *p = pg(k);
  for (u32 i = 0; i < PAGE / 4; i++)
    p[i] = word(k, i, g);
  stats();
}

static u32 wrong_page(u32 k, u32 g) {
  const u32 *p = pg(k);
  u32 w = 0;
  for (u32 i = 0; i < PAGE / 4; i++)
    w += p[i] != word(k, i, g);
  stats();
  return w;
}

static u32 check(void) {
  u32 c = mx__vm_check();
  mx_dbg_word(7, c);
  if (c)
    mx_printf("vm17: mx__vm_check %08x\n", c);
  return c;
}

static u8 tstack[16384] MX_ALIGNED(16);
static volatile u32 other_bad, other_done;
static void other(void *arg) {
  (void)arg;
  for (u32 k = 0; k < 16; k++)
    other_bad += wrong_page(k, gen[k]) != 0;
  other_done = 1;
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  mx_dbg_step(1);
  mx_thread_set_prio(mx_thread_self(), 64);
  mx_dbg_word(2, (u32)mx_thread_prio(mx_thread_self()));
  mx_printf("vm17: 1 mx_vm_init, clean_frames 4\n");
  static const struct mx_vm_config cfg = {
      .swap = "host:vm17swap.bin", .frames = 16 * PAGE, .clean_frames = 4};
  CHECK_EQ(mx_vm_init(&cfg), 0);

  mx_dbg_step(2);
  mx_printf("vm17: 2 %u pages written\n", NP);
  for (u32 k = 0; k < NP; k++)
    write_page(k, gen[k] = 1);
  u32 bad = 0;
  for (u32 k = 0; k < NP; k++)
    bad += wrong_page(k, 1) != 0;
  CHECK_EQ(bad, 0);

  mx_dbg_step(3);
  mx_printf("vm17: 3 400 random touches\n");
  u32 x = 3, inconsistent = 0;
  bad = 0;
  for (u32 n = 0; n < 400 && !inconsistent; n++) {
    mx_dbg_word(1, n);
    x = x * 1103515245u + 12345u;
    u32 k = (x >> 16) % NP;
    if (x >> 30 & 1)
      write_page(k, ++gen[k]);
    else if (wrong_page(k, gen[k])) {
      bad++;
      mx_printf("vm17: page %u wrong at round %u\n", k, n);
    }
    mx_delay_us(10000);
    if (n % 50 == 49)
      inconsistent = check();
  }
  CHECK_EQ(bad, 0);
  CHECK_EQ(inconsistent, 0);

  mx_dbg_step(4);
  mx_printf("vm17: 4 a second thread\n");
  int t = mx_thread_start(other, 0, tstack, sizeof tstack, 64);
  CHECK(t >= 0);
  for (u32 k = 16; k < 32; k++) {
    write_page(k, ++gen[k]);
    mx_thread_yield();
  }
  if (t >= 0)
    mx_thread_join(t);
  CHECK_EQ(other_done, 1);
  CHECK_EQ(other_bad, 0);
  bad = 0;
  for (u32 k = 0; k < NP; k++)
    bad += wrong_page(k, gen[k]) != 0;
  CHECK_EQ(bad, 0);
  CHECK_EQ(check(), 0);

  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("vm17: %u swap ins, %u outs by faults, %u by the writer, %u drops, %u faults\n",
            s.swap_ins, s.swap_outs, s.writer_outs, s.drops, s.faults);
  mx_printf("vm17: ms in swap ins %u, outs %u, the VM lock %u; %u file server calls\n",
            s.us_swap_in / 1000, s.us_swap_out / 1000, s.us_lock / 1000, s.io_calls);
  CHECK(s.writer_outs > 0);

  /* pinned memory: 100 KiB out of the pool (frames move), back when freed */
  struct mx_vm_stats p0, p1, p2;
  mx_vm_stats(&p0);
  void *p = mx_heap_alloc(mx_heap_pinned, 100 * 1024, 64);
  CHECK(p != 0);
  mx_vm_stats(&p1);
  mx_heap_free(mx_heap_pinned, p);
  mx_vm_stats(&p2);
  mx_printf("vm17: pinned frames %u, with 100 KiB more %u, free frames %u -> %u\n",
            p0.pinned_frames, p1.pinned_frames, p0.free_frames, p1.free_frames);
  CHECK(p1.pinned_frames - p0.pinned_frames >= 4);
  CHECK(p2.pinned_frames <= p0.pinned_frames + 1); /* the heap's table: a frame */
  mx_dbg_heartbeat(0);
  return t_done("vm17");
}
