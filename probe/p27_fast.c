/* mxio (common/include/mxio.h) on a fresh IOP: reads a file into EE memory and
   checks every word, then writes the same words to another, read back. With the
   fast path, a host running ps2client serve.

     p27_fast ip=<ip>,<mask>,<gateway> m=poweroff,ps2dev9,netman,smap,ps2ip-nm,mxio
              file=host:fast.bin wfile=host:fastw.bin

   Modules are host:<name>.irx, in order; the stack gets ip=.

   The file holds word i = i * 2654435761 ^ 0x5A, MIB MiB of it; raw=<path> only
   times reading a file of any kind. Results go to the host, as the reset ended the
   console. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>
#include "../common/include/mxio.h"

#define MAX 16
#define MIB 8u

static u8 mem[1024 * 1024] MX_ALIGNED(64);
static struct {
  u8 *at;
  u32 size;
  char arg[64];
  u32 arg_len;
} mod[MAX];
static u32 data[256 * 1024 / 4 + 16] MX_ALIGNED(64);
static u32 req[80] MX_ALIGNED(64);
static s32 ans[4] MX_ALIGNED(64);
static u8 frames[MXIO_BUFFER] MX_ALIGNED(64);
static struct mx_sif_client c;
static char ip_arg[64];
static u32 ip_len;

static int call(int fno, u32 bytes) {
  volatile s32 *r = MX_UNCACHED(ans);
  r[0] = -1000;
  mx_dcache_writeback(req, sizeof req);
  if (mx_sif_call(&c, fno, req, (bytes + 15) & ~15u, (void *)r, 16) < 0)
    return -1001;
  return r[0];
}

static char *cat(char *d, const char *s) {
  while (*s)
    *d++ = *s++;
  *d = 0;
  return d;
}
static void log_text(const char *s) {
  char *d = (char *)req;
  u32 n = 0;
  while (s[n] && n < 250)
    d[n] = s[n], n++;
  d[n] = 0;
  call(MXIO_LOG, n + 1);
}

static int open_file(const char *path, u32 flags, u32 *stride, u32 *piece) {
  req[0] = flags;
  char *d = cat((char *)(req + 1), path);
  int fd = call(MXIO_OPEN, (u32)(d - (char *)req) + 1);
  const volatile s32 *r = MX_UNCACHED(ans);
  if (stride)
    *stride = (u32)r[1], *piece = (u32)r[2];
  return fd;
}

/* the whole file in reads of chunk bytes; returns the words that are wrong */
static u32 pass(int fd, u32 chunk, u32 bytes, int check) {
  u32 bad = 0, cycles = 0, ee = (u32)data & 0x1FFFFFFFu;
  const volatile u32 *w = MX_UNCACHED(data);
  for (u32 at = 0; at < bytes; at += chunk) {
    u32 want = bytes - at < chunk ? bytes - at : chunk;
    req[0] = (u32)fd, req[1] = at, req[2] = want, req[3] = ee;
    u32 t0 = mx_count();
    int got = call(MXIO_READ, 16);
    cycles += mx_count() - t0;
    if (got != (int)want) {
      char line[80];
      mx_snprintf(line, sizeof line, "read failed at %u: %d\n", at, got);
      log_text(line);
      return ~0u;
    }
    for (u32 i = 0; check && i < chunk / 4; i++) {
      u32 n = at / 4 + i;
      bad += w[i] != (n * 2654435761u ^ 0x5Au);
    }
  }
  char line[96];
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_snprintf(line, sizeof line, "p27: reads of %u: %u ms, %u KiB/s, wrong words %u\n", chunk, ms,
              bytes / 1024u * 1000u / ms, bad);
  log_text(line);
  return bad;
}

/* The same words to a file, in writes of 32 KiB */
static int write_pass(int fd, u32 stride, u32 piece) {
  u32 cycles = 0, chunk = 32 * 1024;
  u32 *w = data;
  for (u32 at = 0; at < MIB * 1024 * 1024; at += chunk) {
    for (u32 i = 0; i < chunk / 4; i++)
      w[i] = (at / 4 + i) * 2654435761u ^ 0x5Au;
    u32 t0 = mx_count();
    u32 *args = (u32 *)frames;
    args[0] = (u32)fd, args[1] = at, args[2] = chunk;
    u32 n = (chunk + piece - 1) / piece;
    for (u32 i = 0; i < n; i++) {
      u32 off = i * piece, len = chunk - off > piece ? piece : chunk - off;
      __builtin_memcpy(frames + MXIO_HEAD + i * stride, (u8 *)w + off, len);
    }
    u32 bytes = MXIO_HEAD + (n - 1) * stride + piece;
    if (bytes > sizeof frames)
      bytes = sizeof frames;
    mx_dcache_writeback(frames, bytes);
    volatile s32 *r = MX_UNCACHED(ans);
    r[0] = -1000;
    int got = mx_sif_call(&c, MXIO_WRITE, frames, bytes, (void *)r, 16) < 0 ? -1001 : r[0];
    cycles += mx_count() - t0;
    if (got != (int)chunk) {
      char line[80];
      mx_snprintf(line, sizeof line, "write failed at %u: %d\n", at, got);
      log_text(line);
      return 1;
    }
  }
  char line[96];
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_snprintf(line, sizeof line, "p27: writes of %u: %u ms, %u KiB/s\n", chunk, ms,
              MIB * 1024u * 1000u / ms);
  log_text(line);
  return 0;
}

/* The same through mx_fio_*, as a program uses it */
#define FAIL(what)                                                                         \
  do {                                                                                     \
    log_text("p27: FAILED: " what "\n");                                                   \
    return 1;                                                                              \
  } while (0)

static int fio_test(const char *file, const char *wfile) {
  u8 *b = (u8 *)data;
  u32 bytes = MIB * 1024 * 1024, cycles = 0;
  char line[96];
  int fd = mx_fio_open(file, MX_FIO_RDONLY);
  if (fd < 64)
    FAIL("open did not go through mxio");
  if (mx_fio_lseek(fd, 0, MX_FIO_SEEK_END) != (int)bytes)
    FAIL("size");
  if (mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0)
    FAIL("seek");
  for (u32 at = 0; at < bytes; at += 32768) {
    u32 t0 = mx_count();
    if (mx_fio_read(fd, b, 32768) != 32768)
      FAIL("read");
    cycles += mx_count() - t0;
    for (u32 i = 0; i < 8192; i++)
      if (data[i] != ((at / 4 + i) * 2654435761u ^ 0x5Au))
        FAIL("words");
  }
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_snprintf(line, sizeof line, "p27: mx_fio_read: %u ms, %u KiB/s\n", ms, bytes / 1024u * 1000u / ms);
  log_text(line);
  /* not aligned, an odd size: word 250 starts at byte 1000 */
  if (mx_fio_lseek(fd, 1000, MX_FIO_SEEK_SET) != 1000 || mx_fio_read(fd, b + 3, 5001) != 5001)
    FAIL("unaligned read");
  for (u32 i = 0; i < 5001; i++) {
    u32 at = 1000 + i, w = (at / 4) * 2654435761u ^ 0x5Au;
    if (b[3 + i] != (u8)(w >> (at % 4 * 8)))
      FAIL("unaligned bytes");
  }
  /* aligned with a tail, then the next read goes on from there */
  if (mx_fio_lseek(fd, 64, MX_FIO_SEEK_SET) != 64 || mx_fio_read(fd, b, 1000) != 1000 ||
      mx_fio_lseek(fd, 0, MX_FIO_SEEK_CUR) != 1064)
    FAIL("tail");
  if (mx_fio_lseek(fd, -10, MX_FIO_SEEK_END) != (int)bytes - 10 || mx_fio_read(fd, b, 100) != 10)
    FAIL("end of file");
  if (mx_fio_read(fd, b, 100) != 0)
    FAIL("past the end");
  if (mx_fio_close(fd) < 0)
    FAIL("close");
  if (mx_fio_open("host:no-such-file", MX_FIO_RDONLY) >= 0)
    FAIL("a missing file");

  fd = mx_fio_open(wfile, 3 | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 64)
    FAIL("open to write");
  cycles = 0;
  for (u32 at = 0; at < bytes; at += 32768) {
    for (u32 i = 0; i < 8192; i++)
      data[i] = (at / 4 + i) * 2654435761u ^ 0x5Au;
    u32 t0 = mx_count();
    /* odd pieces at first, then as the pager does */
    if (!at ? mx_fio_write(fd, b, 1001) || mx_fio_write(fd, b + 1001, 31767)
            : mx_fio_write(fd, b, 32768))
      FAIL("write");
    cycles += mx_count() - t0;
  }
  ms = cycles / (MX_EE_HZ / 1000u);
  mx_snprintf(line, sizeof line, "p27: mx_fio_write: %u ms, %u KiB/s\n", ms, bytes / 1024u * 1000u / ms);
  log_text(line);
  if (mx_fio_close(fd) < 0)
    FAIL("close after writing");
  return 0;
}

static int starts(const char *s, const char *with) {
  while (*with)
    if (*s++ != *with++)
      return 0;
  return 1;
}

int main(int argc, char **argv) {
  u8 *to = mem;
  int n = 0;
  const char *file = 0, *wfile = 0, *raw = 0;
  if (mx_sif_init() < 0)
    return 1;
  for (int i = 1; i < argc && n < MAX; i++) {
    if (starts(argv[i], "ip=")) {
      for (const char *p = argv[i] + 3; ip_len < sizeof ip_arg - 2; p++) {
        ip_arg[ip_len++] = *p == ',' ? 0 : *p;
        if (!*p)
          break;
      }
    } else if (starts(argv[i], "m=")) {
      /* the modules, host:<name>.irx, in order */
      for (const char *p = argv[i] + 2; *p && n < MAX;) {
        char path[40], *d = cat(path, "host:");
        const char *name = d;
        while (*p && *p != ',')
          *d++ = *p++;
        *d = 0;
        if (*p)
          p++;
        int stack = starts(name, "ps2ip");
        cat(d, ".irx");
        int fd = mx_fio_open(path, MX_FIO_RDONLY);
        int size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
        if (size <= 0 || to + size > mem + sizeof mem ||
            mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 || mx_fio_read(fd, to, (u32)size) != size) {
          mx_printf("p27: cannot read %s\n", path);
          MX_TEST_END(1);
          return 1;
        }
        mx_fio_close(fd);
        mod[n].at = to, mod[n].size = (u32)size;
        if (stack) {
          __builtin_memcpy(mod[n].arg, ip_arg, ip_len);
          mod[n].arg_len = ip_len;
        }
        to += ((u32)size + 63) & ~63u;
        n++;
      }
    } else if (starts(argv[i], "file=")) {
      file = argv[i] + 5;
    } else if (starts(argv[i], "wfile=")) {
      wfile = argv[i] + 6;
    } else if (starts(argv[i], "raw=")) {
      raw = argv[i] + 4;
    }
  }
  if (!n || (!file && !raw)) {
    mx_printf("p27: need modules and file= or raw=\n");
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("p27: %d modules read, resetting the IOP\n", n);
  if (mx_iop_reset("") < 0)
    return 2;
  mx_iop_patch_lmb();
  mx_iop_patch_prefix_check();
  for (int i = 0; i < n; i++) {
    int res = 0;
    if (mx_iop_load_module_buffer(mod[i].at, mod[i].size, mod[i].arg_len ? mod[i].arg : 0,
                                  mod[i].arg_len, &res) < 0)
      return 3;
  }
  if (mx_sif_bind_tries(&c, MXIO_SID, 1000) < 0 || !c.server)
    return 4;

  /* the link comes up a few seconds after smap starts: a file on the host opens
     through the fast path once the host has said HELLO */
  u32 bad = 0;
  int fd = -1;
  const char *path = file ? file : raw;
  for (int t = 0; t < 20 && fd < 0; t++) {
    fd = open_file(path, MX_FIO_RDONLY, 0, 0);
    if (fd < 0)
      mx_delay_us(1000000);
  }
  if (fd < 0)
    return 5;
  req[0] = (u32)fd;
  int size = call(MXIO_SIZE, 16);
  if (size <= 0)
    return 6;
  bad = pass(fd, 256 * 1024, (u32)size, !raw);
  if (!bad)
    bad = pass(fd, 32 * 1024, (u32)size, !raw);
  req[0] = (u32)fd;
  call(MXIO_CLOSE, 16);
  if (wfile) {
    u32 stride = 0, piece = 0;
    int wfd = open_file(wfile, 3 | MX_FIO_CREAT | MX_FIO_TRUNC, &stride, &piece);
    if (wfd < 0) {
      log_text("p27: cannot open the file to write\n");
      bad = 1;
    } else {
      bad += (u32)write_pass(wfd, stride, piece);
      req[0] = (u32)wfd;
      call(MXIO_CLOSE, 16);
    }
  }
  if (wfile && !bad)
    bad += (u32)fio_test(path, wfile);
  int st = call(MXIO_STATS, 16);
  const volatile s32 *r = MX_UNCACHED(ans);
  char line[96];
  mx_snprintf(line, sizeof line, "p27: frames %d, doubles %d, drops %d, asked again %d\n", st, r[1],
              r[2], r[3]);
  log_text(line);
  log_text(bad ? "p27: FAILED\n" : "p27: passed\n");
  return bad ? 30 : 0;
}
