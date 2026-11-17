/* Beam racing that loses the graphics and gets them back (mx OS plan step
   6, part 4): this program draws in 1080i by passes (mx_hires_init), then
   starts itself as a child, which draws frames of its own in front, and
   then draws on. The drawing hardware's interrupts are the process in
   front's only, so the passes' pipeline is paused at the handover
   (mx__frame_yield) and starts again at the next frame; before, the
   frames after the child waited for a pass's end that never came. The
   lost function is called once.
     k74_hireslost.elf     (status 0 expected; the TV shows 1080i, then the
                            child's frames, then 1080i again) */
#include "t.h"
#include <mx/draw.h>
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/hires.h>
#include <mx/irq.h>
#include <mx/thread.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>
#include <mx/video.h>

MX_CORE_HEAP(512 << 10);

static u8 rec[3 * 128 * 1024] MX_FAST_BSS MX_ALIGNED(64);
static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_FAST_BSS;

static u32 lost_calls;
static void lost(void *arg) {
  (void)arg;
  lost_calls++;
}
static const struct mx_draw_ops ops = {lost, 0};

/* n frames of beam racing; the frames done after them */
static u32 hires_frames(u32 n, u32 r, u32 g, u32 b) {
  for (u32 i = 0; i < n; i++) {
    mx_frame_begin_n();
    mx_hires_clear(r, g, b);
    mx_frame_end();
  }
  return mx_gs.frames_done;
}

int main(int argc, char **argv) {
  if (argc > 1 && !strcmp(argv[1], "child")) {
    int pal = mx_video_default() == &mx_video_pal;
    mx_gs_init_mode(640, pal ? 512 : 448, pal ? MX_GS_PAL : 0);
    mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
    for (int i = 0; i < 60; i++) {
      mx_frame_begin(0x00, 0x60, 0x00);
      mx_frame_end();
    }
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 38);
  CHECK(mx_hires_init(&mx_video_1080i, 960, 540, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000,
                      0x2000) >= 0);
  CHECK(mx_draw_open(&ops) >= 0);
  u32 d0 = hires_frames(30, 0x40, 0x10, 0x10);
  u32 p0 = mx_hires_stats.passes;

  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0), st = -1;
  CHECK(id > 0);
  u32 v0 = mx_irq_intc_count(MX_INTC_VBON);
  CHECK_EQ(mx_proc_wait(id, &st, 0), 1);
  CHECK_EQ(st, 5);
  u32 v1 = mx_irq_intc_count(MX_INTC_VBON);
  mx_sleep_us(500000);
  u32 v2 = mx_irq_intc_count(MX_INTC_VBON);
  mx_printf("k74: vblank handler calls: %u at the child's start, %u at its end, %u half a second later\n",
            v0, v1, v2);

  u32 t0 = mx_count();
  u32 d1 = hires_frames(60, 0x10, 0x10, 0x40);
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  u32 p1 = mx_hires_stats.passes;
  mx_printf("k74: %u frames done before the child, %u after; passes %u then %u; 60 frames in %u ms; lost %u\n",
            d0, d1, p0, p1, ms, lost_calls);
  CHECK(d1 >= d0 + 50);
  CHECK(p1 > p0);
  CHECK(ms < 5000);
  CHECK_EQ(lost_calls, 1);
  return t_done("k74");
}
