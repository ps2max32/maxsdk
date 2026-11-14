/* mx OS plan step 4d: three processes take turns in front. This program (A)
   starts B in front, brings itself in front (B paused), starts C in front,
   then brings B in front (C paused). B ends: C is in front again; C ends:
   A is. A then asks for both exit statuses. Started by mxlink:
     k49_three.elf host:vm32_side.elf     (status 0 expected; B and C are both
       that program) */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

MX_CORE_HEAP(512 << 10);

static int state_of(int id) {
  struct mx_proc_info p[8];
  int n = mx_proc_list(p, 8);
  for (int i = 0; i < n && i < 8; i++)
    if (p[i].id == id)
      return p[i].state;
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    mx_printf("k49: usage: k49_three.elf <program> [args]\n");
    return t_done("k49");
  }
  static char args[256];
  u32 n = 0;
  for (int i = 1; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  int a = mx_proc_self();
  int b = mx_proc_spawn(argc - 1, args, n, 0);
  CHECK(b > 0);
  mx_sleep_us(200000);
  CHECK_EQ(mx_proc_front(a), 0); /* B paused */
  int c = mx_proc_spawn(argc - 1, args, n, 0);
  CHECK(c > 0);
  mx_printf("k49: A %d, B %d, C %d\n", a, b, c);
  mx_sleep_us(200000);
  CHECK_EQ(state_of(a), MX_PROC_BESIDE);
  CHECK_EQ(state_of(b), MX_PROC_WAITING);
  CHECK_EQ(state_of(c), MX_PROC_FRONT);
  CHECK_EQ(mx_proc_front(b), 0); /* C paused, B in front */
  CHECK_EQ(state_of(b), MX_PROC_FRONT);
  CHECK_EQ(state_of(c), MX_PROC_WAITING);
  /* B ends, then C, then this one is in front: wait for each */
  int st = -1;
  while (mx_proc_wait(b, &st, 1) == 0)
    mx_sleep_us(50000);
  CHECK_EQ(st, 0);
  mx_printf("k49: B ended; C %d\n", state_of(c));
  CHECK_EQ(state_of(c), MX_PROC_FRONT);
  while (mx_proc_wait(c, &st, 1) == 0)
    mx_sleep_us(50000);
  CHECK_EQ(st, 0);
  while (state_of(a) != MX_PROC_FRONT)
    mx_sleep_us(50000);
  mx_printf("k49: C ended; A in front\n");
  return t_done("k49");
}
