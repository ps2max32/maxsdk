/* mx_frame_present with vsync: 120 frames of a moving box through the MFIFO,
   each shown at a vblank (VIF1 waits at the present's I-bit MARK until the
   frame is drawn, gs.c's present_release). Checks they were shown. */
#include "t.h"
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/video.h>

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);

static u64 *box(u64 *c, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

int main(void) {
  int pal = mx_video_default() == &mx_video_pal;
  u32 h = pal ? 512 : 448;
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  for (u32 n = 0; n < 120; n++) {
    mx_frame_begin(0x10, 0x20, 0x40);
    u32 x = 100 + n * 3;
    mx_frame_commit(box(mx_frame_direct(5), x, 200, x + 40, 240, 0x30C0F0));
    mx_frame_present();
    mx_frame_end();
  }
  mx_frame_sync();
  mx_printf("fpresent: %u frames, %u shown, %u vblanks\n", mx_gs.frames, mx_gs.shown, mx_gs.vsync);
  CHECK(mx_gs.shown >= 100);
  return t_done("fpresent");
}
