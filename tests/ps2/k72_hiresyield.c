/* A launch after beam racing (a shell that drew 1080i, mxgl_suspend, then
   mx_proc_spawn): frames in chain mode, then beam racing (mx_hires_init),
   then mx_frame_stop, then a child started. mx_proc_spawn's yield must not
   wait on the chain's queue: beam racing released VIF1's channel, so its
   end interrupt never comes (2026-11-17, MXSHELL's launch of Quake III
   hung in mx_frame_sync). The child ends at once.
     k72_hiresyield.elf     (status 0 expected; 1080i on the TV a moment) */
#include "t.h"
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/hires.h>
#include <mx/kernel.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/video.h>

static u8 chain[64 << 10] MX_FAST_BSS MX_ALIGNED(64);
static u8 rec[3 * 128 * 1024] MX_FAST_BSS MX_ALIGNED(64);

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child"))
    return 5;
  CHECK(mx__kernel && mx__kernel->version >= 38);
  int pal = mx_video_default() == &mx_video_pal;
  mx_gs_init_mode(640, pal ? 512 : 448, pal ? MX_GS_PAL : 0);
  CHECK_EQ(mx_frame_init_chain(chain, sizeof chain, (void *)0x70002000, 0x2000), 0);
  for (int i = 0; i < 10; i++) {
    mx_frame_begin(0x20, 0x20, 0x60);
    mx_frame_end();
  }
  CHECK(mx_hires_init(&mx_video_1080i, 960, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000,
                      0x2000) >= 0);
  for (int i = 0; i < 10; i++) {
    mx_frame_begin_n();
    mx_hires_clear(0x30, 0x10, 0x10);
    mx_frame_end();
  }
  mx_frame_stop();
  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  u32 t0 = mx_count();
  int id = mx_proc_spawn(2, args, n + 6, 0), st = -1;
  CHECK(id > 0);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 5);
  mx_printf("k72: the child started and ended in %u ms\n", (mx_count() - t0) / (MX_EE_HZ / 1000));
  return t_done("k72");
}
