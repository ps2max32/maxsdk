/* Ending a paused program that drew by beam racing (MXSHELL's Close of a
   paused Quake III, 2026-11-17, hung the PS2): this program starts itself as
   a child, which draws in 1080i by passes (mx_hires_init) and then lets
   this one in front again (mx_proc_front, as the home button does: its
   passes' pipeline paused); this one then ends it (mx_proc_kill: in front
   first, then ended by the kernel) and waits for it.
     k77_killpaused.elf     (status 0 expected; 1080i a moment) */
#include "t.h"
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/hires.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/video.h>

MX_CORE_HEAP(512 << 10);

static u8 rec[3 * 128 * 1024] MX_FAST_BSS MX_ALIGNED(64);

int main(int argc, char **argv) {
  if (argc > 2 && !strcmp(argv[1], "child")) {
    int parent = 0;
    for (const char *c = argv[2]; *c >= '0' && *c <= '9'; c++)
      parent = parent * 10 + (*c - '0');
    if (mx_hires_init(&mx_video_1080i, 960, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000,
                      0x2000) < 0)
      return 7;
    for (int i = 0; i < 30; i++) {
      mx_frame_begin_n();
      mx_hires_clear(0x40, 0x10, 0x40);
      mx_frame_end();
    }
    mx_printf("k77: child: %u frames, the parent (%d) in front\n", mx_gs.frames_done, parent);
    mx_proc_front(parent); /* paused here until in front again */
    for (;;) { /* not ended: draws on */
      mx_frame_begin_n();
      mx_hires_clear(0x10, 0x40, 0x10);
      mx_frame_end();
    }
  }
  CHECK(mx__kernel && mx__kernel->version >= 40);
  int me = mx_proc_self();
  CHECK(me > 0);
  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  u32 m = n + 6 + (u32)mx_snprintf(args + n + 6, 8, "%d", me) + 1;
  int id = mx_proc_spawn(3, args, m, 0), st = -1;
  CHECK(id > 0);
  /* beside the child until it let this one in front (its 30 frames) */
  struct mx_proc_info pi[8];
  int state = 0;
  for (int t = 0; t < 100 && state != MX_PROC_WAITING; t++) {
    mx_sleep_us(50000);
    int np = mx_proc_list(pi, 8);
    for (int i = 0; i < np; i++)
      if (pi[i].id == id)
        state = pi[i].state;
  }
  int waited = mx_proc_wait(id, &st, 1);
  mx_printf("k77: in front again (child %d: state %d, wait %d); ending it\n", id, state, waited);
  CHECK_EQ(waited, 0);
  u32 t0 = mx_count();
  CHECK_EQ(mx_proc_kill(id, 3), 0);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  mx_printf("k77: the child ended with %d in %u ms\n", st, (mx_count() - t0) / (MX_EE_HZ / 1000));
  CHECK_EQ(st, 3);
  return t_done("k77");
}
