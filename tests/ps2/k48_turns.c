/* mx OS plan step 4d: processes take turns in front (kernel version 24,
   mx_proc_front). This program (libmxk with its core at a run of its own)
   starts the other in front (spawn, it goes on beside), brings itself in
   front (the other paused: waiting in the list), works there, then brings
   the other back, which pauses this one until the other ends; then it asks
   for the other's exit status. Started by mxlink:
     k48_turns.elf host:vm32_side.elf     (status 0 expected) */
#include "t.h"
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>

MX_CORE_HEAP(512 << 10);

#define N 4096
static u32 pattern[N];

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
    mx_printf("k48: usage: k48_turns.elf <program> [args]\n");
    return t_done("k48");
  }
  for (u32 i = 0; i < N; i++)
    pattern[i] = i * 2654435761u;
  static char args[256];
  u32 n = 0;
  for (int i = 1; i < argc; i++) {
    u32 l = (u32)strlen(argv[i]) + 1;
    if (n + l > sizeof args)
      break;
    memcpy(args + n, argv[i], l);
    n += l;
  }
  int me = mx_proc_self();
  int other = mx_proc_spawn(argc - 1, args, n, 0);
  mx_printf("k48: this one %d, %s %d\n", me, argv[1], other);
  CHECK(other > 0);
  mx_sleep_us(300000);
  CHECK_EQ(state_of(other), MX_PROC_FRONT);
  CHECK_EQ(state_of(me), MX_PROC_BESIDE);

  /* this one in front, the other paused */
  CHECK_EQ(mx_proc_front(me), 0);
  CHECK_EQ(state_of(me), MX_PROC_FRONT);
  CHECK_EQ(state_of(other), MX_PROC_WAITING);
  int st = -1;
  CHECK_EQ(mx_proc_wait(other, &st, 1), 0); /* it runs (paused) */
  mx_printf("k48: in front, %s waits\n", argv[1]);
  mx_sleep_us(300000);
  u32 bad = 0;
  for (u32 i = 0; i < N; i++)
    bad += pattern[i] != i * 2654435761u;
  CHECK_EQ(bad, 0);

  /* the other back in front: this one paused until it ends */
  CHECK_EQ(mx_proc_front(other), 0);
  mx_printf("k48: in front again\n");
  CHECK_EQ(state_of(me), MX_PROC_FRONT);
  CHECK_EQ(mx_proc_wait(other, &st, 0), 1);
  CHECK_EQ(st, 0);
  for (u32 i = 0; i < N; i++)
    bad += pattern[i] != i * 2654435761u;
  CHECK_EQ(bad, 0);
  return t_done("k48");
}
