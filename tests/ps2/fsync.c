/* mx_frame_sync and mx_gs_download in the middle of a frame and between
   frames, for an MFIFO stream (mx_frame_init) and a chain stream with a
   PATH3 stream next to it (mx_frame_init_chain, mx_frame3_init).

   Each mode draws a frame (in mx_gs_init's buffers, Z24): the clear, box A;
   syncs and reads A back; draws box C over part of A (the stream goes on
   after the sync: in the MFIFO its drain is started again), syncs and reads
   C back; ends the frame (with box B on PATH3 in chain mode), syncs and
   reads B. Then the buffer is read as CT24 (packed: 3 bytes a pixel) and
   the Z buffer as Z24, and sizes that cannot be read are refused. */

#include "t.h"
#include <mx/frame.h>

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static u8 slots[64 * 1024] MX_ALIGNED(64);
static u8 slots3[64 * 1024] MX_ALIGNED(64);
static u32 px[32] MX_ALIGNED(64);
static u8 row[128 * 3] MX_ALIGNED(64);

#define RGB(r, g, b) ((u32)(r) | (u32)(g) << 8 | (u32)(b) << 16)
static const u32 BG = RGB(0x10, 0x20, 0x30), A = RGB(0xF0, 0, 0), B = RGB(0, 0xF0, 0),
                 C = RGB(0, 0, 0xF0);

static u64 *box(u64 *c, const struct mx_rt *f, int ctx, u32 x0, u32 y0, u32 x1, u32 y1, u32 z,
                u32 rgb) {
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, ctx, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16, 0x80, 1));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x0), mx_rt_py(f, y0), z));
  return mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(mx_rt_px(f, x1), mx_rt_py(f, y1), z));
}

static u32 pixel(const struct mx_rt *f, u32 x, u32 y) {
  px[x & 31] = 0xDEADBEEF;
  if (mx_gs_download(px, MX_GS_PAGE(f->fbp), f->fbw, MX_GS_PSM_CT32, x & ~31u, y, 32, 1) != 0)
    return 0xFFFFFFFFu;
  return px[x & 31] & 0xFFFFFF;
}

static void run(int chain) {
  const char *mode = chain ? "chain + PATH3" : "MFIFO";
  if (chain) {
    CHECK_EQ(mx_frame_init_chain(slots, sizeof slots, (void *)0x70000000, 0x2000), 0);
    CHECK_EQ(mx_frame3_init(slots3, sizeof slots3, (void *)0x70002000, 0x2000), 0);
  } else {
    mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  }
  u32 k = mx_frame_begin_n();
  const struct mx_rt *f = &mx_gs.rt[k & 1];
  mx_frame_target(f, 0);
  u64 *c = mx_frame_direct(3);
  c = mx_e_ad_tag(c, 2, 1);
  c = mx_e_ad(c, MX_GS_REG_PRMODECONT, 1);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_DTHE, 0));
  mx_frame_commit(mx_rt_e_clear(mx_frame_direct(5), f, 0, BG & 0xFF, BG >> 8 & 0xFF, BG >> 16, 1));
  c = mx_frame_direct(2);
  c = mx_e_ad_tag(c, 1, 1);
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1)));
  if (chain) {
    mx_frame3_target(f);
    c = mx_frame3_reserve(2);
    c = mx_e_ad_tag(c, 1, 1);
    mx_frame3_commit(mx_e_ad(c, MX_GS_REG_TEST_2, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1)));
    mx_frame3_commit(box(mx_frame3_reserve(5), f, 1, 200, 20, 260, 60, 100, B));
  }
  mx_frame_commit(box(mx_frame_direct(5), f, 0, 20, 20, 80, 60, 100, A));

  /* in the middle of the frame: A drawn */
  CHECK_EQ(mx_frame_sync(), 0);
  u32 a = pixel(f, 50, 40);
  CHECK_EQ(a, A);
  /* the stream goes on: C over the right half of A */
  mx_frame_commit(box(mx_frame_direct(5), f, 0, 50, 20, 80, 60, 100, C));
  CHECK_EQ(mx_frame_sync(), 0);
  u32 ac = pixel(f, 30, 40), cc = pixel(f, 60, 40);
  CHECK_EQ(ac, A);
  CHECK_EQ(cc, C);
  mx_frame_end();

  /* between frames: everything of the frame drawn, PATH3 too */
  CHECK_EQ(mx_frame_sync(), 0);
  u32 bg = pixel(f, 10, 40), b = pixel(f, 230, 40);
  CHECK_EQ(bg, BG);
  if (chain)
    CHECK_EQ(b, B);

  /* CT24: packed, 3 bytes a pixel; 128 pixels are 384 bytes */
  for (u32 i = 0; i < sizeof row; i++)
    row[i] = 0xEE;
  CHECK_EQ(mx_gs_download(row, MX_GS_PAGE(f->fbp), f->fbw, MX_GS_PSM_CT24, 0, 40, 128, 1), 0);
  int bad24 = 0;
  for (u32 x = 0; x < 128; x++) {
    u32 want = x < 20 || x >= 80 ? BG : x < 50 ? A : C;
    u32 got = RGB(row[3 * x], row[3 * x + 1], row[3 * x + 2]);
    bad24 += got != want;
  }
  CHECK_EQ(bad24, 0);
  /* Z24: the boxes at Z 100 */
  for (u32 i = 0; i < sizeof row; i++)
    row[i] = 0xEE;
  CHECK_EQ(mx_gs_download(row, MX_GS_PAGE(f->zbp), f->fbw, MX_GS_PSM_Z24, 0, 40, 128, 1), 0);
  u32 z = (u32)row[3 * 30] | (u32)row[3 * 30 + 1] << 8 | (u32)row[3 * 30 + 2] << 16;
  CHECK_EQ(z, 100);

  mx_printf("fsync %s: A %06x, A/C %06x/%06x, bg %06x, B %06x, CT24 bad %d, Z24 %u\n", mode, a,
            ac, cc, bg, b, bad24, z);

  u32 t0 = mx_count();
  while (mx_gs.shown < mx_gs.frames && mx_count() - t0 < 300000000u)
    ;
  CHECK_EQ(mx_gs.shown, mx_gs.frames);
  mx_frame_stop();
}

int main(void) {
  mx_gs_init(640, 224);
  run(0);
  run(1);
  /* sizes that cannot be read: 32 CT24 pixels are 96 bytes, not a psm */
  CHECK_EQ(mx_gs_download(row, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT24, 0, 0, 32, 1),
           -1);
  CHECK_EQ(mx_gs_download(row, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, 0x3F, 0, 0, 128, 1), -1);
  return t_done("fsync");
}
