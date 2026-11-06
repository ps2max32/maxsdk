/* Probe: what one READ and one WRITE call of the file server cost on host:
   by size, 4 KiB to 256 KiB (the swap file's transfers, ee/src/vm/vm.c; today
   READs go in 16 KiB pieces, FIO_READ_CHUNK, and WRITEs in 64 KiB,
   FIO_CHUNK, ee/src/base/sif.c). Each size in one call (mx__fio_read_chunk,
   mx__fio_write_chunk), over 1 MiB of a 4 MiB file:
   - sequential: one seek, then the calls one after another (the cached
     file position: no seek between them);
   - random: a seek and a call at a random place each time (a page fault);
   with the data read checked. Prints µs a call, MB/s, and the fixed cost
   and rate fitted over the sizes. Small sizes first: a size the IOP side
   cannot take stalls the run there. */

#include <mx/mx.h>
#include <mx/sif.h>

#define KB 1024u
#define FILE_BYTES (4096 * KB)
#define SPAN (1024 * KB)
#define BIG (256 * KB)

extern u32 mx__fio_read_chunk, mx__fio_write_chunk;
extern volatile u32 mx__fio_calls;

static u32 buf[BIG / 4] MX_ALIGNED(64);
static int fd;

static u32 W(u32 i) { return i * 2654435761u ^ 0x5Au; }
static u32 us(u32 cycles) { return cycles / (MX_EE_HZ / 1000000u); }

static u32 x = 7;
static u32 rnd(u32 n) {
  x = x * 1103515245u + 12345u;
  return (x >> 8) % n;
}

/* fills buf with the file's words from byte at */
static void fill(u32 at, u32 n) {
  for (u32 i = 0; i < n / 4; i++)
    buf[i] = W(at / 4 + i);
}
static u32 wrong(u32 at, u32 n) {
  u32 w = 0;
  for (u32 i = 0; i < n / 4; i++)
    w += buf[i] != W(at / 4 + i);
  return w;
}

struct res {
  u32 us_call, calls;
};

/* n calls of size sz, sequential or random; write or read. Returns µs
   per call (the seek included when random). */
static struct res run(u32 sz, int write, int random, u32 *bad) {
  u32 n = SPAN / sz, at = 0, c0 = mx__fio_calls, t = 0;
  if (!random)
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  for (u32 k = 0; k < n; k++) {
    if (random)
      at = rnd(FILE_BYTES / sz) * sz;
    if (write)
      fill(at, sz);
    u32 t0 = mx_count();
    if (random)
      mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET);
    int r = write ? mx_fio_write(fd, buf, sz) : mx_fio_read(fd, buf, sz);
    t += mx_count() - t0;
    if (write ? r != 0 : r != (int)sz)
      ++*bad;
    else if (!write)
      *bad += wrong(at, sz) != 0;
    if (!random)
      at += sz;
  }
  return (struct res){us(t) / n, (mx__fio_calls - c0) / n};
}

int main(void) {
  fd = mx_fio_open("host:p23.bin", 3 /* RDWR */ | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("p23: cannot open host:p23.bin\n");
    MX_TEST_END(1);
    return 1;
  }
  mx__fio_write_chunk = 64 * KB;
  for (u32 at = 0; at < FILE_BYTES; at += 64 * KB) {
    fill(at, 64 * KB);
    mx_fio_write(fd, buf, 64 * KB);
  }
  static const u32 sizes[] = {4 * KB, 8 * KB, 16 * KB, 32 * KB, 64 * KB, 128 * KB, 256 * KB};
  const u32 ns = sizeof sizes / sizeof sizes[0];
  u32 bad = 0, rd[2][8], wr[2][8];
  mx_printf("p23: one call of each size; us a call (calls it took), MB/s\n");
  for (u32 i = 0; i < ns; i++) {
    u32 sz = sizes[i], b = 0;
    mx__fio_read_chunk = mx__fio_write_chunk = sz;
    struct res r0 = run(sz, 0, 0, &b), r1 = run(sz, 0, 1, &b);
    struct res w0 = run(sz, 1, 0, &b), w1 = run(sz, 1, 1, &b);
    rd[0][i] = r0.us_call, rd[1][i] = r1.us_call, wr[0][i] = w0.us_call, wr[1][i] = w1.us_call;
    mx_printf("p23: %3u KiB: READ seq %6u us (%u) %5u KB/s, random %6u us (%u); "
              "WRITE seq %6u us (%u) %5u KB/s, random %6u us (%u); %u bad\n",
              sz / KB, r0.us_call, r0.calls, r0.us_call ? sz * 1000u / r0.us_call : 0,
              r1.us_call, r1.calls, w0.us_call, w0.calls,
              w0.us_call ? sz * 1000u / w0.us_call : 0, w1.us_call, w1.calls, b);
    bad += b;
  }
  /* fixed cost and µs per KiB, least squares over the sizes (sequential) */
  for (int k = 0; k < 2; k++) {
    u32 (*t)[8] = k ? wr : rd;
    float sx = 0, sy = 0, sxx = 0, sxy = 0;
    for (u32 i = 0; i < ns; i++) {
      float xk = (float)(sizes[i] / KB), y = (float)t[0][i];
      sx += xk, sy += y, sxx += xk * xk, sxy += xk * y;
    }
    float slope = (ns * sxy - sx * sy) / (ns * sxx - sx * sx), fixed = (sy - slope * sx) / ns;
    mx_printf("p23: %s: %d us a call + %d ns a byte (%d KB/s)\n", k ? "WRITE" : "READ",
              (int)fixed, (int)(slope * 1000.0f / 1024.0f), (int)(1e6f / slope));
  }
  mx__fio_read_chunk = 16 * KB;
  mx__fio_write_chunk = 64 * KB;
  mx_fio_close(fd);
  mx_printf("p23: %u calls wrong or short\n", bad);
  MX_TEST_END(bad ? 1 : 0);
  return bad != 0;
}
