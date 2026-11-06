/* mxlink's fast path (tools/mxlink/iop/mxnet/mxfast.h) on a fresh IOP: reads a
   file from ps2client serve into EE memory, past lwIP, and checks every word.

     p27_fast host:poweroff.irx host:ps2dev9.irx host:netman.irx host:smap.irx
              ip=<ip>,<mask>,<gateway> host:ps2ip-nm.irx host=<a.b.c.d> file=<name>

   The file holds word i = i * 2654435761 ^ 0x5A, MIB MiB of it. The reset ends
   host: and the console, so results go to the server, which prints them. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define MAX 8
#define SID 0x4D584601
#define MIB 8u
enum { RPC_OPEN, RPC_READ, RPC_CLOSE, RPC_LOG, RPC_STATS };

static u8 mem[512 * 1024] MX_ALIGNED(64);
static struct {
  u8 *at;
  u32 size;
  char arg[64];
  u32 arg_len;
} mod[MAX];
static u32 data[256 * 1024 / 4 + 16] MX_ALIGNED(64);
static u32 req[80] MX_ALIGNED(64);
static s32 ans[4] MX_ALIGNED(64);
static struct mx_sif_client c;
static u32 host;

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
  req[0] = host;
  char *d = (char *)(req + 1);
  u32 n = 0;
  while (s[n] && n < 250)
    d[n] = s[n], n++;
  d[n] = 0;
  call(RPC_LOG, 4 + n + 1);
}

/* the whole file in reads of chunk bytes; returns the words that are wrong */
static u32 pass(int fd, u32 chunk) {
  u32 bad = 0, cycles = 0, ee = (u32)data & 0x1FFFFFFFu;
  const volatile u32 *w = MX_UNCACHED(data);
  for (u32 at = 0; at < MIB * 1024 * 1024; at += chunk) {
    req[0] = (u32)fd, req[1] = at, req[2] = chunk, req[3] = ee;
    u32 t0 = mx_count();
    int got = call(RPC_READ, 16);
    cycles += mx_count() - t0;
    if (got != (int)chunk) {
      char line[80];
      mx_snprintf(line, sizeof line, "read failed at %u: %d\n", at, got);
      log_text(line);
      return ~0u;
    }
    for (u32 i = 0; i < chunk / 4; i++) {
      u32 n = at / 4 + i;
      bad += w[i] != (n * 2654435761u ^ 0x5Au);
    }
  }
  char line[96];
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_snprintf(line, sizeof line, "p27: reads of %u: %u ms, %u KiB/s, wrong words %u\n", chunk, ms,
              MIB * 1024u * 1000u / ms, bad);
  log_text(line);
  return bad;
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
  const char *file = 0;
  if (mx_sif_init() < 0)
    return 1;
  for (int i = 1; i < argc && n < MAX; i++) {
    if (starts(argv[i], "ip=")) {
      u32 l = 0;
      for (const char *p = argv[i] + 3; l < sizeof mod[n].arg - 2; p++) {
        mod[n].arg[l++] = *p == ',' ? 0 : *p;
        if (!*p)
          break;
      }
      mod[n].arg_len = l;
    } else if (starts(argv[i], "host=")) {
      u32 part = 0, shift = 0;
      for (const char *p = argv[i] + 5;; p++) {
        if (*p == '.' || !*p) {
          host |= part << shift, shift += 8, part = 0;
          if (!*p)
            break;
        } else
          part = part * 10 + (u32)(*p - '0');
      }
    } else if (starts(argv[i], "file=")) {
      file = argv[i] + 5;
    } else {
      int fd = mx_fio_open(argv[i], MX_FIO_RDONLY);
      int size = fd < 0 ? -1 : mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
      if (size <= 0 || to + size > mem + sizeof mem || mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET) != 0 ||
          mx_fio_read(fd, to, (u32)size) != size) {
        mx_printf("p27: cannot read %s\n", argv[i]);
        MX_TEST_END(1);
        return 1;
      }
      mx_fio_close(fd);
      mod[n].at = to, mod[n].size = (u32)size;
      to += ((u32)size + 63) & ~63u;
      n++;
    }
  }
  if (!n || !host || !file) {
    mx_printf("p27: need modules, host= and file=\n");
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
  if (mx_sif_bind_tries(&c, SID, 1000) < 0 || !c.server)
    return 4;

  /* the link comes up a few seconds after smap starts */
  int fd = -1;
  for (int t = 0; t < 15 && fd < 0; t++) {
    req[0] = host;
    char *d = cat((char *)(req + 1), file);
    fd = call(RPC_OPEN, (u32)(d - (char *)req) + 1);
  }
  if (fd < 0)
    return 5;
  u32 bad = pass(fd, 256 * 1024);
  if (!bad)
    bad = pass(fd, 32 * 1024);
  int st = call(RPC_STATS, 16);
  const volatile s32 *r = MX_UNCACHED(ans);
  char line[96];
  mx_snprintf(line, sizeof line, "p27: frames %d, doubles %d, drops %d, asked again %d\n", st, r[1],
              r[2], r[3]);
  log_text(line);
  req[0] = (u32)fd;
  call(RPC_CLOSE, 16);
  log_text(bad ? "p27: FAILED\n" : "p27: passed\n");
  return bad ? 30 : 0;
}
