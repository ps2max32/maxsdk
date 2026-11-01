/* VM step 18: the trace (mx_vm_config.trace): the TLB maps 8 KiB blocks,
   each interval (100 ms) starts with a tick and the VM's entries out of
   the TLB, and the first touch of each block in an interval is recorded,
   read or written. 8 frames (256 KiB), a swap file. Under 30 s; at the end
   the screen is bright green with 900D0000 in slot 0, or bright red with
   BAD000nn.

   Slot 0 and the colour: the step. Slots 4-6: swap outs, swap ins, refills.
     1 mx_vm_init with a trace
     2 known touches, one interval each (a mark before each):
       a: page 0 block 0 read twice, page 1 block 3 read
       b: page 0 block 1 written, page 0 block 0 read (again: a new interval)
       c: page 2 block 2 read, then written (written: seen only with dirty
          tracking, the PS2; PCSX2 ignores D)
     3 24 pages written whole and checked, twice (paging with 8 KiB TLB
       entries), mx__vm_check after each round
     4 the trace saved to host:vm18.trc, read back and checked: its head,
       the ticks in order, the events of step 2 between their marks */

#include "t.h"
#include <mx/sif.h>
#include <mx/timer.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define NP 24
#define BLK 8192u

u32 mx__vm_check(void);

static u8 *pg(u32 k) { return (u8 *)mx_vm_base() + k * PAGE; }
static u32 word(u32 k, u32 i, u32 g) { return k * 2654435761u ^ i * 40503u ^ g; }
static volatile u32 sink;

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.swap_outs);
  mx_dbg_word(5, s.swap_ins);
  mx_dbg_word(6, s.refills);
}

static void rd(u32 k, u32 b) { sink += *(volatile u32 *)(pg(k) + b * BLK + 64); }
static void wr(u32 k, u32 b) { *(volatile u32 *)(pg(k) + b * BLK + 128) = 0x1234u + k; }
static void next_interval(u32 mark) {
  mx_delay_us(150000); /* a tick at least */
  mx_vm_trace_mark(mark);
}

static u32 ev[16384];

/* The events between mark m and the next mark: how many of (kind, block) */
static u32 count(u32 n, u32 m, u32 kind, u32 bi) {
  u32 i = 0, c = 0;
  while (i < n && ev[i] != (MX_VM_TR_MARK << 30 | m))
    i++;
  for (i++; i < n && ev[i] >> 30 != MX_VM_TR_MARK; i++)
    c += ev[i] == (kind << 30 | bi);
  return c;
}

int main(void) {
  mx_dbg_screen();

  mx_dbg_step(1);
  mx_printf("vm18: 1 mx_vm_init with a trace\n");
  static const struct mx_vm_config cfg = {
      .swap = "host:vm18swap.bin", .frames = 8 * PAGE, .trace = 64 * 1024, .trace_ms = 100};
  CHECK_EQ(mx_vm_init(&cfg), 0);
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  int dbit = (int)s.dirty_tracking;

  mx_dbg_step(2);
  mx_printf("vm18: 2 known touches\n");
  next_interval(1);
  rd(0, 0), rd(0, 0), rd(1, 3);
  next_interval(2);
  wr(0, 1), rd(0, 0);
  next_interval(3);
  rd(2, 2), wr(2, 2);
  next_interval(4);

  mx_dbg_step(3);
  mx_printf("vm18: 3 %u pages written and checked, twice\n", NP);
  u32 bad = 0;
  for (u32 g = 1; g <= 2; g++) {
    for (u32 k = 0; k < NP; k++) {
      u32 *p = (u32 *)pg(k);
      for (u32 i = 0; i < PAGE / 4; i++)
        p[i] = word(k, i, g);
      stats();
    }
    for (u32 k = 0; k < NP; k++) {
      const u32 *p = (const u32 *)pg(k);
      for (u32 i = 0; i < PAGE / 4; i++)
        bad += p[i] != word(k, i, g);
      stats();
    }
    u32 c = mx__vm_check();
    if (c)
      mx_printf("vm18: mx__vm_check %08x\n", c);
    CHECK_EQ(c, 0);
  }
  CHECK_EQ(bad, 0);

  mx_dbg_step(4);
  mx_printf("vm18: 4 the trace saved and read back\n");
  mx_vm_trace_mark(5);
  int n = mx_vm_trace_save("host:vm18.trc");
  CHECK(n > 0);
  int fd = mx_fio_open("host:vm18.trc", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  struct mx_vm_trace_head h = {0};
  if (fd >= 0) {
    CHECK_EQ(mx_fio_read(fd, &h, sizeof h), sizeof h);
    u32 want = h.events < 16384 ? h.events : 16384;
    CHECK_EQ(mx_fio_read(fd, ev, want * 4), (int)(want * 4));
    mx_fio_close(fd);
  }
  CHECK_EQ(h.magic, MX_VM_TRACE_MAGIC);
  CHECK_EQ(h.page, PAGE);
  CHECK_EQ(h.block, BLK);
  CHECK_EQ(h.nframes, 8);
  CHECK_EQ(h.interval_ms, 100);
  CHECK_EQ(h.events, (u32)n);
  CHECK_EQ(h.lost, 0);
  u32 ne = h.events < 16384 ? h.events : 16384, ticks = 0, last = 0, order = 1;
  for (u32 i = 0; i < ne; i++)
    if (ev[i] >> 30 == MX_VM_TR_TICK) {
      u32 ms = ev[i] & 0x3FFFFFFFu;
      order &= !ticks || ms >= last + 100;
      last = ms, ticks++;
    }
  mx_printf("vm18: %u events, %u ticks, the last at %u ms\n", ne, ticks, last);
  CHECK(ticks >= 4);
  CHECK_EQ(order, 1);
  /* a: block 0 once (read twice), block 7 */
  CHECK_EQ(count(ne, 1, MX_VM_TR_READ, 0), 1);
  CHECK_EQ(count(ne, 1, MX_VM_TR_READ, 1 * 4 + 3), 1);
  CHECK_EQ(count(ne, 1, MX_VM_TR_WRITE, 0), 0);
  /* b: block 1 written, block 0 read again */
  CHECK_EQ(count(ne, 2, MX_VM_TR_WRITE, 1), 1);
  CHECK_EQ(count(ne, 2, MX_VM_TR_READ, 0), 1);
  /* c: block 10 read, then written */
  CHECK_EQ(count(ne, 3, MX_VM_TR_READ, 2 * 4 + 2), 1);
  CHECK_EQ(count(ne, 3, MX_VM_TR_WRITE, 2 * 4 + 2), dbit ? 1 : 0);
  /* step 3 touched every block of its pages */
  u32 blocks = 0;
  for (u32 bi = 0; bi < NP * 4; bi++)
    blocks += count(ne, 4, MX_VM_TR_WRITE, bi) > 0;
  CHECK_EQ(blocks, NP * 4);
  return t_done("vm18");
}
