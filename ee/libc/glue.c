/* picolibc on maxsdk (phase 8, D8c): stdio on the debug output, the heap
   from SetupHeap, exit through mx_exit. Built into libmxc.a; programs that
   use libc link crt0, their objects, libmxc.a, picolibc's libc.a, libmx.a
   and compiler-rt's builtins (Makefile, LIBC_LIBS). */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/times.h>
#include <unistd.h>

#include <mx/cpu.h>
#include <mx/debug.h>

void mx_exit(int status) __attribute__((noreturn));

/* ---- stdio ---------------------------------------------------------------- */

static char line[256];
static unsigned line_len;

static int out_flush(FILE *f) {
  (void)f;
  if (line_len)
    mx_debug_write(line, line_len);
  line_len = 0;
  return 0;
}

static int out_put(char c, FILE *f) {
  line[line_len++] = c;
  if (c == '\n' || line_len == sizeof line)
    out_flush(f);
  return (unsigned char)c;
}

static FILE out = FDEV_SETUP_STREAM(out_put, NULL, out_flush, _FDEV_SETUP_WRITE);
FILE *const stdin = &out;
FILE *const stdout = &out;
FILE *const stderr = &out;

ssize_t write(int fd, const void *buf, size_t n) {
  if (fd != 1 && fd != 2) {
    errno = EBADF;
    return -1;
  }
  out_flush(&out);
  mx_debug_write(buf, (unsigned)n);
  return (ssize_t)n;
}

ssize_t read(int fd, void *buf, size_t n) {
  (void)fd, (void)buf, (void)n;
  errno = EBADF;
  return -1;
}

/* ---- time --------------------------------------------------------------------- */

/* EE cycles since start: COP0 Count extended to 64 bits at each call
   (calls must be less than 14.5 s apart). There is no real-time clock:
   time() counts from 0 at program start. */
static unsigned long long cycles(void) {
  /* read with interrupts off: the core's interrupt part (mx OS plan 4b, step 6) */
  static unsigned last MX_IRQ_BSS, high MX_IRQ_BSS;
  int was = mx_di();
  unsigned c = mx_count();
  if (c < last)
    high++;
  last = c;
  mx_irq_restore(was);
  return (unsigned long long)high << 32 | c;
}

int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  unsigned long long us = cycles() / 295; /* 294.912 MHz */
  tv->tv_sec = (time_t)(us / 1000000);
  tv->tv_usec = (suseconds_t)(us % 1000000);
  return 0;
}

/* clock() uses times(): CLOCKS_PER_SEC is 1000000 */
clock_t times(struct tms *t) {
  clock_t c = (clock_t)(cycles() / 295);
  if (t) {
    t->tms_utime = c;
    t->tms_stime = t->tms_cutime = t->tms_cstime = 0;
  }
  return c;
}

/* ---- heap and exit ----------------------------------------------------------- */

/* The break of <mx/vm.h>: VM when VM is on, physical memory otherwise. */
void *mx_sbrk(s32 incr);

void *sbrk(ptrdiff_t incr) {
  void *p = mx_sbrk((s32)incr);
  if (p == (void *)-1)
    errno = ENOMEM;
  return p;
}

void _exit(int status) {
  out_flush(&out);
  mx_exit(status);
}

/* start.c calls this instead of mx_exit when libc is linked: atexit
   handlers and C++ static destructors run, stdio is flushed. */
void mx__libc_exit(int status) { exit(status); }

/* The working directory (<mx/sif.h>: the kernel's, for each process). Weak:
   a program's own file layer (a port's POSIX files) has its own. */
#include <string.h>
#include <mx/sif.h>
__attribute__((weak)) int chdir(const char *path) {
  if (mx_fio_chdir(path) < 0) {
    errno = ENOENT;
    return -1;
  }
  return 0;
}
__attribute__((weak)) char *getcwd(char *buf, size_t size) {
  char d[MX_FIO_CWD_MAX];
  int n = mx_fio_getcwd(d, sizeof d);
  if (n < 0) {
    errno = ERANGE;
    return NULL;
  }
  if (!buf) {
    size = size > (size_t)n + 1 ? size : (size_t)n + 1;
    if (!(buf = malloc(size)))
      return NULL;
  }
  if (size < (size_t)n + 1) {
    errno = ERANGE;
    return NULL;
  }
  memcpy(buf, d, (size_t)n + 1);
  return buf;
}
