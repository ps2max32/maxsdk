/* Step 2e-3 (docs/mx-os-apps-plan.md): spawn, wait and kill (kernel version
   14). This program (libmxk without VM, its core at a run of its own) starts
   vm34_child in front and goes on at once: while that runs it works and
   asks (wait without waiting), then waits for its exit status (7). It
   starts it again to run until ended, ends it (kill, status 42) and gets
   that. A path that does not load: -1, and this one is in front again.
   Started by mxlink:
     k33_spawn.elf host:vm34_child.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

MX_CORE_HEAP(512 << 10);

static u32 words(char *to, const char *a, const char *b) {
  u32 n = (u32)strlen(a) + 1;
  memcpy(to, a, n);
  if (b) {
    memcpy(to + n, b, strlen(b) + 1);
    n += (u32)strlen(b) + 1;
  }
  return n;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k33: usage: k33_spawn.elf <vm34_child.elf>\n");
    return t_done("k33");
  }
  int self = mx_proc_self(), st = -1;
  static char args[256];

  /* 1: it exits by itself; this one goes on meanwhile */
  u32 n = words(args, argv[1], "exit7");
  int id = mx_proc_spawn(2, args, n, 0);
  mx_printf("k33: process %d started %s: process %d\n", self, argv[1], id);
  CHECK(id > 0 && id != self);
  u32 polls = 0;
  int r;
  while ((r = mx_proc_wait(id, &st, 1)) == 0) {
    polls++;
    mx_sleep_us(5000);
  }
  mx_printf("k33: it ended after %u polls: %d, status %d\n", polls, r, st);
  CHECK_EQ(r, 1);
  CHECK_EQ(st, 7);
  CHECK(polls > 0);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1); /* asked again: the same */
  CHECK_EQ(mx_proc_wait(id + 77, &st, 0), -1);

  /* 2: it runs until ended */
  n = words(args, argv[1], "forever");
  id = mx_proc_spawn(2, args, n, 0);
  CHECK(id > 0);
  CHECK_EQ(mx_proc_kill(self, 1), -1); /* not the one in front */
  mx_sleep_us(100000);
  CHECK_EQ(mx_proc_wait(id, &st, 1), 0);
  CHECK_EQ(mx_proc_kill(id, 42), 0);
  st = -1;
  r = mx_proc_wait(id, &st, 0);
  mx_printf("k33: ended it: %d, status %d\n", r, st);
  CHECK_EQ(r, 1);
  CHECK_EQ(st, 42);

  /* 3: a path that does not load */
  n = words(args, "host:no-such.elf", 0);
  r = mx_proc_spawn(1, args, n, 0);
  mx_printf("k33: a missing program: %d\n", r);
  CHECK_EQ(r, -1);
  CHECK_EQ(mx_proc_self(), self);
  return t_done("k33");
}
