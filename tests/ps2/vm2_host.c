/* VM step 2: host: file I/O from the main thread, as the swap file uses
   it, shown on the debug screen. Under a minute; at the end the screen is
   bright green with 900D0000 in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step. Slot 1: the last call's result, slot 2:
   the round (step 6), slot 3: the file descriptor. Heartbeat in slots 30
   and 31 (bottom right).
     1 open host:vm2.bin read-write, created, truncated
     2 write 16 bytes
     3 seek to 0, read them back
     4 32 KiB written and read back at 0
     5 1000 bytes from and to unaligned buffers at offset 100
     6 200 rounds: a seek to one of 16 32-KiB slots, then a write or a
       checked read (slot 2 counts)
     7 128 KiB written in one call and read back in one call
     8 closed, opened read-only, the first 16 bytes read again */

#include "t.h"
#include <mx/sif.h>

#define SLOT 32768u
#define NSLOT 16
#define FAR (1u << 20) /* step 6's slots start here */

static u8 buf[4 * SLOT] MX_ALIGNED(64);
static u8 back[4 * SLOT] MX_ALIGNED(64);
static u32 gen[NSLOT];

static u8 byte(u32 seed, u32 i) { return (u8)((i * 2654435761u ^ seed * 40503u) >> 13); }

static void fill(u8 *p, u32 n, u32 seed) {
  for (u32 i = 0; i < n; i++)
    p[i] = byte(seed, i);
}

static u32 wrong(const u8 *p, u32 n, u32 seed) {
  u32 w = 0;
  for (u32 i = 0; i < n; i++)
    w += p[i] != byte(seed, i);
  return w;
}

static int fd;

static int show(int r) {
  mx_dbg_word(1, (u32)r);
  return r;
}

static void step(u32 n, const char *what) {
  mx_dbg_step(n);
  mx_printf("vm2: %u %s\n", n, what);
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);

  step(1, "open");
  fd = show(mx_fio_open("host:vm2.bin", MX_FIO_RDONLY | MX_FIO_WRONLY | MX_FIO_CREAT |
                                            MX_FIO_TRUNC));
  mx_dbg_word(3, (u32)fd);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("vm2");

  step(2, "write 16");
  fill(buf, 16, 1);
  CHECK_EQ(show(mx_fio_write(fd, buf, 16)), 0);

  step(3, "read 16");
  CHECK_EQ(show(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET)), 0);
  CHECK_EQ(show(mx_fio_read(fd, back, 16)), 16);
  CHECK_EQ(wrong(back, 16, 1), 0);

  step(4, "32 KiB");
  fill(buf, SLOT, 2);
  CHECK_EQ(show(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET)), 0);
  CHECK_EQ(show(mx_fio_write(fd, buf, SLOT)), 0);
  CHECK_EQ(show(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET)), 0);
  CHECK_EQ(show(mx_fio_read(fd, back, SLOT)), (int)SLOT);
  CHECK_EQ(wrong(back, SLOT, 2), 0);

  step(5, "unaligned");
  fill(buf + 4, 1000, 3);
  CHECK_EQ(show(mx_fio_lseek(fd, 100, MX_FIO_SEEK_SET)), 100);
  CHECK_EQ(show(mx_fio_write(fd, buf + 4, 1000)), 0);
  CHECK_EQ(show(mx_fio_lseek(fd, 100, MX_FIO_SEEK_SET)), 100);
  CHECK_EQ(show(mx_fio_read(fd, back + 12, 1000)), 1000);
  CHECK_EQ(wrong(back + 12, 1000, 3), 0);

  step(6, "200 rounds");
  u32 x = 1, bad = 0;
  for (u32 k = 0; k < 200 && !bad; k++) {
    mx_dbg_word(2, k);
    x = x * 1103515245u + 12345u;
    u32 s = (x >> 16) % NSLOT;
    int write = !gen[s] || (x >> 28 & 1);
    if (show(mx_fio_lseek(fd, (s32)(FAR + s * SLOT), MX_FIO_SEEK_SET)) != (int)(FAR + s * SLOT))
      bad = 1;
    else if (write) {
      fill(buf, SLOT, 100 + s * 1000 + ++gen[s]);
      bad = show(mx_fio_write(fd, buf, SLOT)) != 0;
    } else {
      bad = show(mx_fio_read(fd, back, SLOT)) != (int)SLOT ||
            wrong(back, SLOT, 100 + s * 1000 + gen[s]) != 0;
    }
    if (bad)
      mx_printf("vm2: round %u (slot %u, %s) failed\n", k, s, write ? "write" : "read");
  }
  CHECK_EQ(bad, 0);

  step(7, "128 KiB");
  fill(buf, 4 * SLOT, 4);
  CHECK_EQ(show(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET)), 0);
  CHECK_EQ(show(mx_fio_write(fd, buf, 4 * SLOT)), 0);
  CHECK_EQ(show(mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET)), 0);
  CHECK_EQ(show(mx_fio_read(fd, back, 4 * SLOT)), (int)(4 * SLOT));
  CHECK_EQ(wrong(back, 4 * SLOT, 4), 0);

  step(8, "reopen");
  /* PCSX2 answers 0, the PS2 under ps2link 3 (not seen where): either */
  CHECK(show(mx_fio_close(fd)) >= 0);
  fd = show(mx_fio_open("host:vm2.bin", MX_FIO_RDONLY));
  mx_dbg_word(3, (u32)fd);
  CHECK(fd >= 0);
  if (fd >= 0) {
    CHECK_EQ(show(mx_fio_read(fd, back, 16)), 16);
    CHECK_EQ(wrong(back, 16, 4), 0);
    mx_fio_close(fd);
  }
  mx_dbg_heartbeat(0);
  return t_done("vm2");
}
