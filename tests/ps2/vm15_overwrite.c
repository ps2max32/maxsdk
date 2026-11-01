/* VM step 15: mx_fio_read into VM pins with MX_VM_PIN_OVERWRITE: pages it
   overwrites whole are not read in first (no swap read, no zero fill); a
   short read puts the rest back (mx_vm_overwrite_done). 8 frames and a
   swap file. Under a minute; at the end the screen is bright green with
   900D0000 in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 2: a value checked. Slots 4-7:
   pages not read in (overwrites), restores, swap ins, zero fills.
   Heartbeat in slots 30 and 31.
     1 host:vm15.bin written (6.5 pages), 16 VM pages written and pushed
       out to the swap file
     2 5 pages of the file read at byte 100 of page 0: pages 1-4 not read
       in; the bytes around the read as they were
     3 4 pages asked for at page 8, 2.5 left in the file: pages 8-11 not
       read in, the read short, page 10's second half and page 11 back
       from the swap file
     4 new pages (never written): 2 pages read whole (no zero fill), then a
       short read of 1000 bytes into 2 pages (the rest zero) */

#include "t.h"
#include <mx/sif.h>
#include <mx/vm.h>

#define PAGE MX_VM_PAGE
#define FILE_BYTES (6 * PAGE + PAGE / 2)

static const struct mx_vm_config cfg = {.swap = "host:vm15swap.bin", .frames = 8 * PAGE};

static u32 F(u32 i) { return i * 2654435761u ^ 0x3C3C3C3Cu; }
static u32 A(u32 i) { return i * 40503u ^ 0x77777777u; }
static u8 fbyte(u32 o) { return (u8)(F(o >> 2) >> (8 * (o & 3))); }
static u8 abyte(u32 o) { return (u8)(A(o >> 2) >> (8 * (o & 3))); }

static u32 wbuf[PAGE / 4] MX_ALIGNED(64);
static u8 *buf;
static int fd;

static void stats(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_dbg_word(4, s.overwrites);
  mx_dbg_word(5, s.restores);
  mx_dbg_word(6, s.swap_ins);
  mx_dbg_word(7, s.zero_fills);
}

static void step(u32 n, const char *what) {
  stats();
  mx_dbg_step(n);
  mx_printf("vm15: %u %s\n", n, what);
}

static void write_a(u32 from_pg, u32 to_pg) {
  for (u32 i = from_pg * PAGE / 4; i < to_pg * PAGE / 4; i++)
    ((u32 *)buf)[i] = A(i);
}

/* bytes [from, to) of buf: the file from file offset fo, or A */
static u32 wrong_file(u32 from, u32 to, u32 fo) {
  u32 w = 0;
  for (u32 o = from; o < to; o++)
    w += buf[o] != fbyte(fo + o - from);
  return w;
}
static u32 wrong_a(u32 from, u32 to) {
  u32 w = 0;
  for (u32 o = from; o < to; o++)
    w += buf[o] != abyte(o);
  return w;
}

static void push_out(void) { write_a(16, 24); } /* 8 other pages: the frames' worth */

static int read_at(u32 fo, u32 bo, u32 n) {
  if (mx_fio_lseek(fd, (s32)fo, MX_FIO_SEEK_SET) != (int)fo)
    return -1;
  return mx_fio_read(fd, buf + bo, n);
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  struct mx_vm_stats s0, s1;

  step(1, "a file, 16 pages pushed out");
  CHECK_EQ(mx_vm_init(&cfg), 0);
  fd = mx_fio_open("host:vm15.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  for (u32 at = 0; at < FILE_BYTES; at += PAGE) {
    u32 n = FILE_BYTES - at < PAGE ? FILE_BYTES - at : PAGE;
    for (u32 i = 0; i < n / 4; i++)
      wbuf[i] = F(at / 4 + i);
    CHECK_EQ(mx_fio_write(fd, wbuf, n), 0);
  }
  mx_fio_close(fd);
  fd = mx_fio_open("host:vm15.bin", MX_FIO_RDONLY);
  CHECK(fd >= 0);
  buf = mx_sbrk(28 * PAGE);
  CHECK(buf != (void *)-1);
  if (fd < 0 || buf == (void *)-1)
    return t_done("vm15");
  write_a(0, 16);
  push_out();

  step(2, "5 pages read at byte 100");
  mx_vm_stats(&s0);
  CHECK_EQ(read_at(0, 100, 5 * PAGE), (int)(5 * PAGE));
  mx_vm_stats(&s1);
  mx_printf("vm15: %u not read in, %u swap ins\n", s1.overwrites - s0.overwrites,
            s1.swap_ins - s0.swap_ins);
  CHECK_EQ(s1.overwrites - s0.overwrites, 4);
  CHECK(s1.swap_ins - s0.swap_ins <= 2);
  CHECK_EQ(wrong_a(0, 100), 0);
  CHECK_EQ(wrong_file(100, 100 + 5 * PAGE, 0), 0);
  CHECK_EQ(wrong_a(100 + 5 * PAGE, 6 * PAGE), 0);

  step(3, "a short read over 4 pages");
  push_out();
  mx_vm_stats(&s0);
  int got = read_at(4 * PAGE, 8 * PAGE, 4 * PAGE);
  mx_dbg_word(2, (u32)got);
  CHECK_EQ(got, (int)(FILE_BYTES - 4 * PAGE));
  mx_vm_stats(&s1);
  mx_printf("vm15: got %d, %u not read in, %u restored\n", got, s1.overwrites - s0.overwrites,
            s1.restores - s0.restores);
  CHECK_EQ(s1.overwrites - s0.overwrites, 4);
  CHECK_EQ(s1.restores - s0.restores, 2);
  u32 end = 8 * PAGE + (FILE_BYTES - 4 * PAGE);
  CHECK_EQ(wrong_file(8 * PAGE, end, 4 * PAGE), 0);
  CHECK_EQ(wrong_a(end, 12 * PAGE), 0);
  CHECK_EQ(wrong_a(0, 100), 0); /* page 0 from the swap file again */

  step(4, "new pages");
  mx_vm_stats(&s0);
  CHECK_EQ(read_at(0, 24 * PAGE, 2 * PAGE), (int)(2 * PAGE));
  mx_vm_stats(&s1);
  CHECK_EQ(s1.overwrites - s0.overwrites, 2);
  CHECK_EQ(s1.zero_fills - s0.zero_fills, 0);
  CHECK_EQ(wrong_file(24 * PAGE, 26 * PAGE, 0), 0);
  CHECK_EQ(read_at(FILE_BYTES - 1000, 26 * PAGE, 2 * PAGE), 1000);
  CHECK_EQ(wrong_file(26 * PAGE, 26 * PAGE + 1000, FILE_BYTES - 1000), 0);
  u32 nz = 0;
  for (u32 o = 26 * PAGE + 1000; o < 28 * PAGE; o++)
    nz += buf[o] != 0;
  CHECK_EQ(nz, 0);
  mx_fio_close(fd);

  stats();
  mx_vm_stats(&s1);
  mx_printf("vm15: %u not read in, %u restored, %u swap ins, %u outs, %u zero fills\n",
            s1.overwrites, s1.restores, s1.swap_ins, s1.swap_outs, s1.zero_fills);
  mx_dbg_heartbeat(0);
  return t_done("vm15");
}
