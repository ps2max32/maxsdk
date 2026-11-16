/* The memory card driver (mxmc, kernel version 31) writing: in a directory
   of its own on mc0 (MXMCTEST, removed at the end), files of 0 bytes to
   300 KB written and read back, six open at once written in turns, one
   written over shorter, the directory listed; then all removed. Prints
   the speed of the large file. The card's free space (mx_dev_list, from
   mxmc through mxio) goes down by what was written and back at the end.
     k61_mcwrite.elf        (status 0) */
#include <mx/dev.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include "t.h"

#define DIR "mc0:/MXMCTEST"
#define NF 6
static const u32 sizes[NF] = {0, 1, 1023, 1024, 8209, 300000};
static u8 buf[300000] MX_ALIGNED(64), got[300000] MX_ALIGNED(64);

static u8 pat(u32 file, u32 i) { return (u8)(i * 7 + file * 13 + (i >> 9)); }

static void name(char *to, u32 i) { mx_snprintf(to, 40, DIR "/F%u.BIN", i); }

static void fill(u32 f, u32 n) {
  for (u32 i = 0; i < n; i++)
    buf[i] = pat(f, i);
}

static u32 ms_since(u32 t0) { return (mx_count() - t0) / (MX_EE_HZ / 1000); }

/* the file read whole and compared with file f's pattern of n bytes */
static void check(u32 f, u32 n) {
  char p[40];
  name(p, f);
  int fd = mx_fio_open(p, MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd < 0)
    return;
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  CHECK_EQ(size, n);
  memset(got, 0xAA, n + 16 < sizeof got ? n + 16 : sizeof got);
  int r = n ? mx_fio_read(fd, got, n) : 0;
  mx_fio_close(fd);
  CHECK_EQ(r, n);
  u32 bad = 0;
  for (u32 i = 0; i < n; i++)
    bad += got[i] != pat(f, i);
  if (bad)
    mx_printf("k61: %s: %u bytes differ\n", p, bad);
  CHECK_EQ(bad, 0);
}

/* mc0:'s free bytes in the device list, after mxio's next look (each second) */
static u64 dev_free(void) {
  mx_sleep_us(1500000);
  struct mx_dev_info d[8];
  int n = mx_dev_list(d, 8);
  for (int i = 0; i < n && i < 8; i++)
    if (!strcmp(d[i].name, "mc0:") && d[i].present) {
      mx_printf("k61: mc0: %llu of %llu bytes free\n", d[i].free, d[i].size);
      return d[i].free;
    }
  mx_printf("k61: mc0: not in the device list\n");
  return 0;
}

static void clean(void) {
  char p[40];
  for (u32 i = 0; i < NF; i++) {
    name(p, i);
    mx_fio_remove(p);
  }
  mx_fio_rmdir(DIR);
}

int main(void) {
  char p[40];
  clean(); /* a run that ended early */
  u64 free0 = dev_free();
  CHECK(free0 > 0);
  CHECK_EQ(mx_fio_mkdir(DIR, 0777), 0);

  /* one after the other */
  for (u32 f = 0; f < NF; f++) {
    name(p, f);
    fill(f, sizes[f]);
    u32 t0 = mx_count();
    int fd = mx_fio_open(p, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
    CHECK(fd >= 0);
    if (fd < 0)
      continue;
    CHECK_EQ(mx_fio_write(fd, buf, sizes[f]), 0);
    CHECK(mx_fio_close(fd) >= 0);
    if (sizes[f] >= 100000)
      mx_printf("k61: %u bytes written in %u ms\n", sizes[f], ms_since(t0));
  }
  u64 free1 = dev_free();
  CHECK(free1 + 300000 <= free0);
  for (u32 f = 0; f < NF; f++) {
    u32 t0 = mx_count();
    check(f, sizes[f]);
    if (sizes[f] >= 100000)
      mx_printf("k61: %u bytes read in %u ms\n", sizes[f], ms_since(t0));
  }

  /* six open at once, written in turns of 1000 bytes */
  int fd[NF];
  for (u32 f = 0; f < NF; f++) {
    name(p, f);
    fd[f] = mx_fio_open(p, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
    CHECK(fd[f] >= 0);
  }
  for (u32 off = 0; off < 8209; off += 1000)
    for (u32 f = 0; f < NF; f++) {
      if (fd[f] < 0 || off >= sizes[f] || f == NF - 1)
        continue;
      u32 n = sizes[f] - off < 1000 ? sizes[f] - off : 1000;
      static u8 piece[1000] MX_ALIGNED(64);
      for (u32 i = 0; i < n; i++)
        piece[i] = pat(f, off + i);
      CHECK_EQ(mx_fio_write(fd[f], piece, n), 0);
    }
  /* the last one: the large file again, while the others are open */
  fill(NF - 1, sizes[NF - 1]);
  if (fd[NF - 1] >= 0)
    CHECK_EQ(mx_fio_write(fd[NF - 1], buf, sizes[NF - 1]), 0);
  for (u32 f = 0; f < NF; f++)
    if (fd[f] >= 0)
      CHECK(mx_fio_close(fd[f]) >= 0);
  for (u32 f = 0; f < NF; f++)
    check(f, sizes[f]);

  /* written over, shorter (the card's driver, as mcman, knows no TRUNC of
     its own: CREAT makes an existing file new, without it the file keeps
     its length) */
  name(p, NF - 1);
  fill(NF - 1, 5000);
  int w = mx_fio_open(p, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(w >= 0);
  if (w >= 0) {
    CHECK_EQ(mx_fio_write(w, buf, 5000), 0);
    mx_fio_close(w);
  }
  check(NF - 1, 5000);

  /* the directory */
  int d = mx_fio_dopen(DIR);
  CHECK(d >= 0);
  int files = 0;
  if (d >= 0) {
    char n[64];
    u32 size;
    while (mx_fio_dread(d, n, sizeof n, &size) > 0) {
      mx_printf("k61: %s %u\n", n, size);
      files += n[0] == 'F';
    }
    mx_fio_dclose(d);
  }
  CHECK_EQ(files, NF);

  /* removed */
  clean();
  CHECK_EQ(dev_free(), free0);
  d = mx_fio_dopen(DIR);
  CHECK(d < 0);
  if (d >= 0)
    mx_fio_dclose(d);
  return t_done("k61");
}
