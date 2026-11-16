/* What the kernel said about the memory cards: every notification it
   keeps and its log ring as it is now; then for secs seconds (default 20)
   the card read as the settings writer reads it (mc0:/MXOS/SETTINGS.DAT,
   open, read, close, every 250 ms), each failure with its code, and the
   notifications that came meanwhile. Reads only.
     k60_mclog.elf [secs]     (status 0: no failure)
     k60_mclog.elf hold       (status 0) up to 10 card files open at once (as
       many as the card driver gives: mcman 3 for all, mxmc 16), then 3 s: no
       notification of a memory card */
#include <mx/sif.h>
#include <mx/log.h>
#include <mx/notify.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include "t.h"

static void notes(u32 after) {
  static struct mx_notify n[32];
  int k = mx_notify_read(after, n, 32);
  for (int i = 0; i < k; i++)
    mx_printf("k60: note %u at %us: %s: %s\n", n[i].id, n[i].time, n[i].title, n[i].text);
}

static int has(const char *s, const char *w) {
  u32 m = (u32)strlen(w);
  for (; *s; s++)
    if (!strncmp(s, w, m))
      return 1;
  return 0;
}

/* the log's new kernel pieces (proc 0) only, joined into lines */
static u32 pos;
static void log_new(u32 ms) {
  static char line[256];
  static u32 len;
  char piece[72];
  int proc, n;
  while ((n = mx_log_read(&pos, piece, sizeof piece - 1, &proc, 0)) > 0) {
    if (proc)
      continue;
    for (int i = 0; i < n; i++) {
      if (piece[i] == '\n' || len == sizeof line - 1) {
        line[len] = 0;
        /* not the end marker of an earlier program: ps2client would stop at it */
        if (!has(line, "IOP server") && !has(line, "syscalls") && !has(line, " 80 x") &&
            !has(line, "@@PS2TEST_END"))
          mx_printf("k60: %u ms log: %s\n", ms, line);
        len = 0;
      } else
        line[len++] = piece[i];
    }
  }
}

int main(int argc, char **argv) {
  u32 secs = 20;
  if (argc > 1) {
    secs = 0;
    for (const char *p = argv[1]; *p; p++)
      secs = secs * 10 + (u32)(*p - '0');
  }
  if (argc > 1 && !strcmp(argv[1], "hold")) {
    /* how many files of the card open at once (reads) */
    static const char *f[] = {"mc0:/MXOS/SETTINGS.DAT", "mc0:/MXOS/SETTINGS.NEW",
                              "mc0:/MXOS/MXKERNEL.ELF", "mc0:/MXBOOT/BOOT.CFG",
                              "mc0:/MXOS/mxio.irx",     "mc0:/MXOS/sio2man.irx",
                              "mc0:/MXOS/mcserv.irx",   "mc0:/MXOS/padman.irx",
                              "mc0:/MXOS/libsd.irx",    "mc0:/MXOS/mxsnd.irx"};
    enum { N = sizeof f / sizeof f[0] };
    int fd[N], open = 0;
    for (int i = 0; i < N; i++) {
      fd[i] = mx_fio_open(f[i], MX_FIO_RDONLY);
      mx_printf("k60: open %s with %d open: %d\n", f[i], open, fd[i]);
      open += fd[i] >= 0;
    }
    mx_printf("k60: %d of %d files open\n", open, (int)N);
    int d = mx_fio_dopen("mc0:/");
    mx_printf("k60: dopen mc0:/ then: %d\n", d);
    if (d >= 0)
      mx_fio_dclose(d);
    /* the card's files all taken for 3 s: mxio's watcher must not take
       that for a card that went */
    u32 seq = mx_notify_seq();
    mx_sleep_us(3000000);
    static struct mx_notify n[32];
    int k = mx_notify_read(seq, n, 32), went = 0;
    for (int i = 0; i < k; i++) {
      mx_printf("k60: note %u at %us while held: %s: %s\n", n[i].id, n[i].time, n[i].title,
                n[i].text);
      went += has(n[i].title, "emory card") || has(n[i].text, "emory card");
    }
    CHECK_EQ(went, 0);
    for (int i = 0; i < N; i++)
      if (fd[i] >= 0)
        mx_fio_close(fd[i]);
    return t_done("k60");
  }
  notes(0);
  log_new(0);
  u32 seq = mx_notify_seq();
  static char buf[4096];
  int fails = 0;
  for (u32 t = 0; t < secs * 4; t++) {
    int fd = mx_fio_open("mc0:/MXOS/SETTINGS.DAT", MX_FIO_RDONLY);
    int r = fd < 0 ? fd : mx_fio_read(fd, buf, sizeof buf);
    if (fd >= 0)
      mx_fio_close(fd);
    if (fd < 0 || r <= 0) {
      fails++;
      mx_printf("k60: %u ms: open %d read %d\n", t * 250, fd, r);
    }
    log_new(t * 250);
    mx_sleep_us(250000);
  }
  notes(seq);
  mx_printf("k60: %d failures in %u reads\n", fails, secs * 4);
  CHECK_EQ(fails, 0);
  return t_done("k60");
}
