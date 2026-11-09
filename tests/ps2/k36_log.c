/* Step 2e-3 (docs/mx-os-apps-plan.md): the kernel's log (kernel version 16).
   This program (libmxk without VM, its core at a run of its own) prints a
   line, starts vm34_child (which prints its own), and reads the log: both
   lines, each with its process, and the kernel's too. Then it waits on the
   log (wait) for the next child's line. Started by mxlink:
     k36_log.elf host:vm34_child.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/log.h>
#include <mx/proc.h>
#include <mx/string.h>

MX_CORE_HEAP(512 << 10);

static int has(const char *s, u32 n, const char *w) {
  u32 m = (u32)strlen(w);
  for (u32 i = 0; i + m <= n; i++)
    if (!memcmp(s + i, w, m))
      return 1;
  return 0;
}

static int spawn(const char *path, const char *arg) {
  static char args[256];
  u32 n = (u32)strlen(path) + 1;
  memcpy(args, path, n);
  memcpy(args + n, arg, strlen(arg) + 1);
  n += (u32)strlen(arg) + 1;
  return mx_proc_spawn(2, args, n, 0);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k36: usage: k36_log.elf <vm34_child.elf>\n");
    return t_done("k36");
  }
  int self = mx_proc_self(), st;
  mx_printf("k36: a line of process %d\n", self);
  int id = spawn(argv[1], "exit3");
  CHECK(id > 0);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);

  u32 pos = 0, pieces = 0;
  int mine = 0, its = 0, kernel = 0, who;
  char b[80];
  int n;
  while ((n = mx_log_read(&pos, b, sizeof b, &who, 0)) > 0) {
    pieces++;
    mine += who == self && has(b, (u32)n, "k36: a line of process");
    its += who == id && has(b, (u32)n, "vm34: process");
    kernel += who == 0;
  }
  mx_printf("k36: %u pieces up to %u: mine %d, the child's %d, the kernel's %d\n", pieces, pos,
            mine, its, kernel);
  CHECK(mine == 1);
  CHECK(its == 1);
  CHECK(kernel > 0);

  /* waiting on the log for the next child's line */
  id = spawn(argv[1], "exit4");
  CHECK(id > 0);
  int seen = 0;
  for (int i = 0; i < 200 && !seen; i++) {
    n = mx_log_read(&pos, b, sizeof b, &who, 1);
    seen = n > 0 && who == id && has(b, (u32)n, "vm34: process");
  }
  CHECK(seen);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 4);
  return t_done("k36");
}
