/* The picture for the shell at the home button (kernel version 32, mx OS plan
   step 5): what maxsdk's frame code reads back from GS memory when the kernel
   is about to bring the shell in front (src/gs/home.c's mx__home_capture).
   This program draws frames of one colour with a white box, stops the
   stream as at the home button (mx_frame_sync) and takes the picture: its
   size and format, the colour, and the box where it was drawn. The kernel's
   side (mx_home_picture, the shell's only) is in k50_home, run as the shell.
     k64_homepic.elf     (status 0 expected) */
#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/screen.h>
#include <mx/video.h>
#include "t.h"

MX_CORE_HEAP(512 << 10);

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024) MX_IRQ_DATA;
void *mx__home_capture(struct mx_picture *out);

#define BX 100
#define BW 40

static u64 *box(u64 *c, u32 x0, u32 y0, u32 x1, u32 y1, u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x0), mx_gs_py(y0), 0));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x1), mx_gs_py(y1), 0));
}

int main(void) {
  int pal = mx_video_default() == &mx_video_pal;
  u32 h = pal ? 512 : 448, by = h / 2;
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70000000, 0x2000);
  for (u32 n = 0; n < 60; n++) {
    mx_frame_begin(0x40, 0x20, 0x60);
    mx_frame_commit(box(mx_frame_direct(5), BX, by, BX + BW, by + BW, 0xFFFFFF));
    mx_frame_present();
    mx_frame_end();
  }
  CHECK_EQ(mx_frame_sync(), 0);
  struct mx_picture p = {0};
  u8 *m = mx__home_capture(&p);
  CHECK(m != 0);
  if (!m)
    return t_done("k64");
  mx_printf("k64: %u x %u, psm %u, %u bytes, %u display lines a line\n", p.w, p.h, p.psm, p.bytes, p.lines);
  CHECK_EQ(p.w, 640);
  CHECK_EQ(p.h, h);
  CHECK_EQ(p.psm, MX_GS_PSM_CT32);
  CHECK_EQ(p.bytes, 640 * h * 4);
  u32 white = 0, inbox = 0, ground = 0, other = 0;
  for (u32 y = 0; y < p.h; y++)
    for (u32 x = 0; x < p.w; x++) {
      const u8 *q = m + (y * p.w + x) * 4;
      u32 rgb = q[0] | q[1] << 8 | (u32)q[2] << 16;
      if (rgb == 0xFFFFFF) {
        white++;
        inbox += x >= BX && x < BX + BW && y >= by && y < by + BW;
      } else if (rgb == 0x602040)
        ground++;
      else if (other++ < 4)
        mx_printf("k64: (%u, %u) is %06x\n", x, y, rgb);
    }
  mx_printf("k64: %u white (%u in the box), %u of the ground's colour, %u other\n", white, inbox, ground, other);
  CHECK_EQ(white, BW * BW);
  CHECK_EQ(inbox, BW * BW);
  CHECK_EQ(other, 0);
  mx_dma_free(m);
  return t_done("k64");
}
