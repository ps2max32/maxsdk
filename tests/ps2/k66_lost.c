/* A program takes its graphics up again after another had them (kernel
   version 34, mx OS plan step 5b). This program draws frames (the ring
   stream) and takes its picture (mx__home_capture); then it starts itself as
   a child beside it, which is in front, draws other frames with a stream of
   its own for a second and ends: the kernel resets the GIF, the VIFs and
   the DMA channels then. Back in front, the parent's next frame begin finds
   the graphics lost (the kernel's count moved) and sets them up again; it
   draws the same frames and its picture must be the same, pixel for pixel.
   Before version 34 its stream stood still there.
     k66_lost.elf     (status 0 expected) */
#include <mx/cpu.h>
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/proc.h>
#include <mx/screen.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/video.h>
#include "t.h"
#include <mx/draw.h>

MX_CORE_HEAP(512 << 10);

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_IRQ_DATA;
void *mx__home_capture(struct mx_picture *out);

static u64 *box(u64 *c, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

static u32 h;
static void frames(u32 n, u32 r, u32 g, u32 b, u32 x, u32 rgb) {
  for (u32 i = 0; i < n; i++) {
    mx_frame_begin(r, g, b);
    mx_frame_commit(box(mx_frame_direct(5), x, h / 2, x + 60, h / 2 + 60, rgb));
    mx_frame_present();
    mx_frame_end();
  }
}

static u32 lost_calls;
static void lost(void *arg) {
  (void)arg;
  lost_calls++;
}
static const struct mx_draw_ops ops = {lost, 0};

int main(int argc, char **argv) {
  int pal = mx_video_default() == &mx_video_pal;
  h = pal ? 512 : 448;
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  if (argc > 1 && !strcmp(argv[1], "child")) {
    frames(60, 0x70, 0x00, 0x00, 400, 0x00FF00); /* red, a green box elsewhere */
    return 5;
  }
  CHECK(mx__kernel && mx__kernel->version >= 34);
  CHECK(mx_draw_open(&ops) >= 0);
  frames(30, 0x20, 0x40, 0x60, 100, 0xFFFFFF);
  CHECK_EQ(mx_frame_sync(), 0);
  struct mx_picture pa = {0}, pb = {0};
  u8 *a = mx__home_capture(&pa);
  CHECK(a != 0);

  static char args[256];
  u32 n = (u32)strlen(argv[0]) + 1;
  memcpy(args, argv[0], n);
  memcpy(args + n, "child", 6);
  int id = mx_proc_spawn(2, args, n + 6, 0);
  mx_printf("k66: the child is process %d\n", id);
  CHECK(id > 0);
  u32 t0 = mx_count();
  frames(30, 0x20, 0x40, 0x60, 100, 0xFFFFFF); /* the first begin waits for the child's end */
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000);
  int st = -1;
  CHECK_EQ(mx_proc_wait(id, &st, 1), 1);
  CHECK_EQ(st, 5);
  mx_printf("k66: 30 frames after the child in %u ms; the program's own function called %u times\n", ms,
            lost_calls);
  CHECK(lost_calls >= 1);
  CHECK_EQ(mx_frame_sync(), 0);
  u8 *b = mx__home_capture(&pb);
  CHECK(b != 0);
  if (a && b) {
    CHECK_EQ(pa.bytes, pb.bytes);
    u32 diff = 0, red = 0;
    for (u32 i = 0; i < pa.bytes; i += 4) {
      diff += a[i] != b[i] || a[i + 1] != b[i + 1] || a[i + 2] != b[i + 2];
      red += b[i] == 0x70 && b[i + 1] == 0 && b[i + 2] == 0;
    }
    mx_printf("k66: %u of %u pixels differ from the picture before; %u still the child's red\n", diff,
              pa.bytes / 4, red);
    CHECK_EQ(diff, 0);
  }
  return t_done("k66");
}
