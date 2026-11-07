/* The home menu (mx-kernel 7): a program linked into VM on maxsdk's frame
   pipeline (MFIFO), its display written through the kernel. It draws a
   moving box on a background whose colour cycles, and reads the pad each
   frame. With "auto" it draws 240 frames and checks they were shown (the
   display's flips go through the kernel); without, it runs until it is
   ended: hold Select + Start + L1 + R1 for the kernel's menu (resume, the
   pad's settings, quit). The box follows the d-pad; Cross turns it white,
   so the pad's settings can be seen. */

#include "t.h"
#include <mx/frame.h>
#include <mx/kernel.h>
#include <mx/pad.h>
#include <mx/string.h>
#include <mx/video.h>

/* the DMAC reads it: core memory */
static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_CORE_DATA;

static u64 *box(u64 *c, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

int main(int argc, char **argv) {
  int autorun = argc > 1 && !strcmp(argv[1], "auto");
  mx_printf("vm24_home: kernel version %u%s\n", mx__kernel ? mx__kernel->version : 0,
            autorun ? ", 240 frames" : ", hold Select+Start+L1+R1 for the menu");
  int pal = mx_video_default() == &mx_video_pal;
  u32 h = pal ? 512 : 448;
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  CHECK(mx_pad_init() == 0);
  s32 x = 300, y = (s32)h / 2 - 20;
  for (u32 n = 0; !autorun || n < 240; n++) {
    u32 b = mx_pad_buttons(0);
    if (b & MX_PAD_LEFT)
      x -= 4;
    if (b & MX_PAD_RIGHT)
      x += 4;
    if (b & MX_PAD_UP)
      y -= 4;
    if (b & MX_PAD_DOWN)
      y += 4;
    x = x < 0 ? 0 : x > 600 ? 600 : x;
    y = y < 0 ? 0 : y > (s32)h - 40 ? (s32)h - 40 : y;
    u32 t = n % 256, r = t < 128 ? t : 255 - t;
    mx_frame_begin(r / 2, 0x20, 0x60 - r / 4);
    u32 bx = (u32)x + (n % 120 < 60 ? n % 60 : 60 - n % 60);
    mx_frame_commit(box(mx_frame_direct(5), bx, (u32)y, bx + 40, (u32)y + 40,
                        b & MX_PAD_CROSS ? 0xFFFFFF : 0x30C0F0));
    mx_frame_present();
    mx_frame_end();
  }
  mx_frame_sync();
  mx_printf("vm24_home: %u frames, %u shown, %u vblanks\n", mx_gs.frames, mx_gs.shown,
            mx_gs.vsync);
  CHECK(mx_gs.shown >= 200);
  return t_done("vm24_home");
}
