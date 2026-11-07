/* Files of mc0:/MXOS by name, with a read back:

     p32_mcfiles backup             all of them to host:MXOS.bak/ (which must exist)
     p32_mcfiles put host:<file>... host:<file> to mc0:/MXOS/<file>, the old one
                                    replaced, the new one read back and compared */

#include <mx/mx.h>
#include <mx/iop.h>
#include <mx/sif.h>
#include <mx/timer.h>

static const char *const names[] = {
    "chime.irx", "poweroff.irx", "mxdev9.irx", "mxeth.irx", "mxnet.irx", "mxfast.irx",
    "ioptrap.irx", "ps2link.irx", "sio2man.irx", "mcman.irx", "mcserv.irx",
    "padman.irx", "libsd.irx", "audsrv.irx", "mxio.irx", "mxsock.irx", "bdm.irx", "bdmfs_fatfs.irx",
    "usbd.irx", "usbmass_bd.irx", "mx4sio_bd.irx", "MXSHELL.ELF"};

static u8 a[262144] MX_ALIGNED(64), b[262144] MX_ALIGNED(64);

static void cat(char *d, const char *x, const char *y) {
  while (*x)
    *d++ = *x++;
  while (*y)
    *d++ = *y++;
  *d = 0;
}

static int read_all(const char *path, u8 *to) {
  int f = mx_fio_open(path, MX_FIO_RDONLY);
  if (f < 0)
    return f;
  int n = mx_fio_read(f, to, sizeof a);
  mx_fio_close(f);
  return n;
}

static int write_all(const char *path, const u8 *from, int n) {
  int f = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (f < 0)
    return f;
  int r = mx_fio_write(f, from, (u32)n);
  mx_fio_close(f);
  return r;
}

static int backup(void) {
  int missing = 0;
  for (u32 i = 0; i < sizeof names / sizeof *names; i++) {
    char s[64], t[64];
    cat(s, "mc0:/MXOS/", names[i]);
    cat(t, "host:MXOS.bak/", names[i]);
    int n = read_all(s, a);
    if (n <= 0 || n == (int)sizeof a) {
      mx_printf("p32: %s: %d, not saved\n", s, n);
      missing++;
      continue;
    }
    int r = write_all(t, a, n);
    mx_printf("p32: %s %d bytes: %d\n", names[i], n, r);
    if (r)
      return 2;
  }
  mx_printf("p32: %d missing\n", missing);
  return 0;
}

static int put(const char *from) {
  char t[64];
  cat(t, "mc0:/MXOS/", from + 5);
  int n = read_all(from, a);
  if (n <= 0 || n == (int)sizeof a) {
    mx_printf("p32: %s: %d\n", from, n);
    return 3;
  }
  int r = write_all(t, a, n);
  if (r) {
    mx_printf("p32: %s: write %d\n", t, r);
    return 4;
  }
  int got = read_all(t, b), bad = got != n;
  for (int k = 0; k < n && !bad; k++)
    bad = a[k] != b[k];
  mx_printf("p32: %s %d bytes %s\n", t, n, bad ? "BAD" : "ok");
  return bad ? 5 : 0;
}

int main(int argc, char **argv) {
  int r = 1;
  if (argc > 1 && mx_sif_init() >= 0) {
    if (argv[1][0] == 'b')
      r = backup();
    else
      for (int i = 2, s = r = 0; i < argc && !s; i++)
        r = s = put(argv[i]);
  }
  MX_TEST_END(r);
  return r;
}
