/* The graphics are the process in front's (kernel version 33, mx OS plan step
   5a): the frame code of a process beside it waits at its frame boundary
   until it is in front. This program starts itself as a child beside it
   (mx_proc_spawn: the child is in front), which sleeps two seconds and ends;
   the parent goes on with a frame at once, and that frame's begin must not
   return before the child is gone.
     k65_frontwait.elf     (status 0 expected) */
#include <mx/cpu.h>
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/video.h>
#include "t.h"

MX_CORE_HEAP(512 << 10);

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_IRQ_DATA;

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    mx_sleep_us(2000000);
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 33);
  int pal = mx_video_default() == &mx_video_pal;
  mx_gs_init_mode(640, pal ? 512 : 448, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  for (u32 n = 0; n < 30; n++) { /* in front: nothing waits */
    mx_frame_begin(0x20, 0x40, 0x20);
    mx_frame_present();
    mx_frame_end();
  }
  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0);
  mx_printf("k65: the child is process %d\n", id);
  CHECK(id > 0);
  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 1), 0); /* it runs */
  u32 t0 = mx_count();
  mx_frame_begin(0x60, 0x20, 0x20);
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  int ended = mx_proc_wait(id, &st, 1);
  mx_printf("k65: the frame's begin took %u ms; the child %s, status %d\n", ms,
            ended == 1 ? "had ended" : "still ran", st);
  CHECK(ms >= 1500 && ms < 6000);
  CHECK_EQ(ended, 1);
  CHECK_EQ(st, 5);
  /* no more frames: the child's end reset the GIF, the VIFs and the DMA
     channels under this program's stream (the owner ended), and taking the
     graphics up again after another owner is step 5b */
  return t_done("k65");
}
