/* Textures in the top byte of 24-bit buffers (GS manual 2.6.3: writes
   leave the bits a format does not use as they are, so PSMCT24 and PSMT8H
   share an area). A CT24 page (colour) and a Z24 page (depth):
     1 a T8H texture into the colour page's top bytes, a T4HL and a T4HH
       one into the depth page's (two 4-bit textures in a byte)
     2 drawn into both as a framebuffer: a clear sprite (Z ALWAYS), a
       blended sprite with Z GEQUAL and Z writes, a Gouraud triangle with
       varying Z
     3 read back: the top bytes are the textures (T8H), the colour and Z
       what was drawn (CT24, Z24)
     4 the three textures drawn through their CLUTs into a CT32 page: each
       texel's colour
     5 a new T8H texture uploaded over the colour page: its colour stays */

#include "t.h"

static u32 pk[2048] MX_ALIGNED(64);
static u8 t8[64 * 32] MX_ALIGNED(64);
static u8 t4l[64 * 32 / 2] MX_ALIGNED(64), t4h[64 * 32 / 2] MX_ALIGNED(64);
static u32 clut8[256] MX_ALIGNED(64), clut4[16] MX_ALIGNED(64);
static u8 back[64 * 32 * 4] MX_ALIGNED(64);

static u32 pat8(u32 x, u32 y, u32 gen) { return (x * 5 + y * 29 + 3 + gen * 77) & 0xFF; }
static u32 pat4l(u32 x, u32 y) { return (x + y) & 15; }
static u32 pat4h(u32 x, u32 y) { return ((x * 3) ^ y) & 15; }
static u32 col8(u32 i) { return 0x80000000u | (255 - i) << 16 | (i ^ 0x55) << 8 | i; }
static u32 col4(u32 i) { return 0x80000000u | (i * 16) << 16 | (i * 3) << 8 | (255 - i * 9); }

static void send(struct mx_pkt *p) {
  mx_gs_finish_ad(p);
  mx_pkt_end(p, 0, 0);
  CHECK(mx_pkt_finish(p) > 0);
  mx_pkt_writeback(p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(mx_pkt_start(p));
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
}

static void make_t8(u32 gen) {
  for (u32 y = 0; y < 32; y++)
    for (u32 x = 0; x < 64; x++)
      t8[y * 64 + x] = (u8)pat8(x, y, gen);
  mx_dcache_writeback(t8, sizeof t8);
}

/* drawing into 64 x 32 of page fb (psm) with Z in page zb (Z24) */
static void draw_env(struct mx_pkt *p, u32 fb, u32 psm, u32 zb) {
  mx_pkt_ad_begin(p, 6, 0);
  mx_pkt_ad(p, MX_GS_REG_FRAME_1, mx_gs_frame(fb, 1, psm, 0));
  mx_pkt_ad(p, MX_GS_REG_ZBUF_1, mx_gs_zbuf(zb, MX_GS_PSM_Z24, 0));
  mx_pkt_ad(p, MX_GS_REG_XYOFFSET_1, mx_gs_xyoffset(0, 0));
  mx_pkt_ad(p, MX_GS_REG_SCISSOR_1, mx_gs_scissor(0, 63, 0, 31));
  mx_pkt_ad(p, MX_GS_REG_PRMODECONT, 1);
  mx_pkt_ad(p, MX_GS_REG_COLCLAMP, 1);
}

static void sprite(struct mx_pkt *p, u32 x0, u32 x1, u32 rgb, u32 a, u32 z, int abe, u32 ztst) {
  mx_pkt_ad_begin(p, 5, 0);
  mx_pkt_ad(p, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, ztst));
  mx_pkt_ad(p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, abe, 0, 0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb & 0xFF, rgb >> 8 & 0xFF, rgb >> 16 & 0xFF, a, 1));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(x0 << 4, 0, z));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(x1 << 4, 32 << 4, z));
}

/* texture psm at block tbp (64 x 32, buffer width 64) with its CLUT at cbp,
   drawn 1:1 into the CT32 page fb */
static void textured(struct mx_pkt *p, u32 fb, u32 tbp, u32 psm, u32 cbp) {
  draw_env(p, fb, MX_GS_PSM_CT32, fb + 1);
  u64 tex0 = mx_gs_tex0(tbp, 1, psm, 6, 5, 1, 1) | MX_SET(GS_TEX0_CBP, cbp) |
             MX_SET(GS_TEX0_CPSM, MX_GS_PSM_CT32) | MX_SET(GS_TEX0_CLD, 1);
  mx_pkt_ad_begin(p, 8, 0);
  mx_pkt_ad(p, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1));
  mx_pkt_ad(p, MX_GS_REG_TEX0_1, tex0);
  mx_pkt_ad(p, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_UV, mx_gs_uv(0, 0));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_UV, mx_gs_uv(64 << 4, 32 << 4));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(64 << 4, 32 << 4, 0));
}

/* the texels of a page that differ from want(x, y): read as T8H */
static u32 check_top(u32 page, u32 (*want)(u32, u32, u32), u32 gen) {
  CHECK_EQ(mx_gs_download(back, MX_GS_PAGE(page), 1, MX_GS_PSM_T8H, 0, 0, 64, 32), 0);
  u32 bad = 0;
  for (u32 y = 0; y < 32; y++)
    for (u32 x = 0; x < 64; x++)
      if (back[y * 64 + x] != want(x, y, gen)) {
        if (!bad)
          mx_printf("page %u (%u, %u): top byte %02x, expected %02x\n", page, x, y,
                    back[y * 64 + x], want(x, y, gen));
        bad++;
      }
  return bad;
}
static u32 want_t4(u32 x, u32 y, u32 gen) {
  (void)gen;
  return pat4l(x, y) | pat4h(x, y) << 4;
}

int main(void) {
  mx_gs_init(640, 224);
  u32 cp = mx_vram_alloc(1), zp = mx_vram_alloc(1), tp = mx_vram_alloc(2), lp = mx_vram_alloc(1);
  CHECK(cp != ~0u && zp != ~0u && tp != ~0u && lp != ~0u);
  u32 cb8 = MX_GS_PAGE(lp), cb4 = MX_GS_PAGE(lp) + 4;
  mx_printf("hbyte: colour page %u, depth page %u\n", cp, zp);

  /* the CLUTs: T8's in CSM1 order (bits 3 and 4 of the index swapped) */
  for (u32 i = 0; i < 256; i++)
    clut8[(i & 0xE7) | (i & 8) << 1 | (i & 16) >> 1] = col8(i);
  for (u32 i = 0; i < 16; i++)
    clut4[i] = col4(i);
  for (u32 y = 0; y < 32; y++)
    for (u32 x = 0; x < 64; x += 2) {
      t4l[(y * 64 + x) / 2] = (u8)(pat4l(x, y) | pat4l(x + 1, y) << 4);
      t4h[(y * 64 + x) / 2] = (u8)(pat4h(x, y) | pat4h(x + 1, y) << 4);
    }
  make_t8(0);
  mx_dcache_writeback(clut8, sizeof clut8);
  mx_dcache_writeback(clut4, sizeof clut4);
  mx_dcache_writeback(t4l, sizeof t4l);
  mx_dcache_writeback(t4h, sizeof t4h);

  /* 1 */
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, MX_GS_PAGE(cp), 1, MX_GS_PSM_T8H, 0, 0, 64, 32, t8);
  mx_gs_upload(&p, MX_GS_PAGE(zp), 1, MX_GS_PSM_T4HL, 0, 0, 64, 32, t4l);
  mx_gs_upload(&p, MX_GS_PAGE(zp), 1, MX_GS_PSM_T4HH, 0, 0, 64, 32, t4h);
  mx_gs_upload(&p, cb8, 1, MX_GS_PSM_CT32, 0, 0, 16, 16, clut8);
  mx_gs_upload(&p, cb4, 1, MX_GS_PSM_CT32, 0, 0, 8, 2, clut4);
  send(&p);
  CHECK_EQ(check_top(cp, pat8, 0), 0);
  CHECK_EQ(check_top(zp, want_t4, 0), 0);

  /* 2 */
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  draw_env(&p, cp, MX_GS_PSM_CT24, zp);
  sprite(&p, 0, 64, 0x332211, 0x80, 0x654321, 0, 1); /* the clear */
  mx_pkt_ad_begin(&p, 1, 0);
  mx_pkt_ad(&p, MX_GS_REG_ALPHA_1, mx_gs_alpha(0, 1, 0, 1, 0));
  sprite(&p, 0, 32, 0xFFFFFF, 0x40, 0x800000, 1, 2); /* left half: blended, Z written */
  mx_pkt_ad_begin(&p, 8, 0);
  mx_pkt_ad(&p, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  mx_pkt_ad(&p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRIANGLE, 1, 0, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(255, 0, 0, 0x80, 1));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(32 << 4, 0, 0xFFFFFF));
  mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0, 255, 0, 0x80, 1));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(64 << 4, 0, 0x700000));
  mx_pkt_ad(&p, MX_GS_REG_RGBAQ, mx_gs_rgbaq(0, 0, 255, 0x80, 1));
  mx_pkt_ad(&p, MX_GS_REG_XYZ2, mx_gs_xyz(64 << 4, 32 << 4, 0x700000));
  send(&p);

  /* 3 */
  CHECK_EQ(check_top(cp, pat8, 0), 0);
  CHECK_EQ(check_top(zp, want_t4, 0), 0);
  CHECK_EQ(mx_gs_download(back, MX_GS_PAGE(cp), 1, MX_GS_PSM_CT24, 0, 0, 64, 32), 0);
  /* left half blended over the clear: 0x11 + (0xFF - 0x11) / 2 */
  u32 c0 = back[(5 * 64 + 5) * 3], c1 = back[(5 * 64 + 60) * 3 + 1];
  mx_printf("hbyte: colour left %02x (blended), lower right green %02x\n", c0, c1);
  CHECK(c0 > 0x11 + 0x60 && c0 < 0x11 + 0x90);
  CHECK(c1 > 0x40); /* in the triangle: mostly green there */
  CHECK_EQ(back[(31 * 64 + 40) * 3], 0x11); /* below the triangle: the clear */
  CHECK_EQ(mx_gs_download(back, MX_GS_PAGE(zp), 1, MX_GS_PSM_Z24, 0, 0, 64, 32), 0);
  u32 zl = back[(5 * 64 + 5) * 3] | back[(5 * 64 + 5) * 3 + 1] << 8 | back[(5 * 64 + 5) * 3 + 2] << 16;
  u32 zb = back[(31 * 64 + 40) * 3] | back[(31 * 64 + 40) * 3 + 1] << 8 |
           back[(31 * 64 + 40) * 3 + 2] << 16;
  mx_printf("hbyte: Z left %06x, lower right %06x\n", zl, zb);
  CHECK_EQ(zl, 0x800000);
  CHECK_EQ(zb, 0x654321);
  u32 zt = back[(5 * 64 + 60) * 3] | back[(5 * 64 + 60) * 3 + 1] << 8 | back[(5 * 64 + 60) * 3 + 2] << 16;
  mx_printf("hbyte: Z in the triangle %06x\n", zt);
  CHECK(zt > 0x700000 && zt < 0xFFFFFF); /* between its corners: 700000 .. FFFFFF */

  /* 4 */
  const struct {
    u32 page, psm, cbp;
    const char *name;
  } tex[3] = {{cp, MX_GS_PSM_T8H, cb8, "T8H"},
              {zp, MX_GS_PSM_T4HL, cb4, "T4HL"},
              {zp, MX_GS_PSM_T4HH, cb4, "T4HH"}};
  for (u32 i = 0; i < 3; i++) {
    mx_pkt_begin(&p, pk, sizeof pk, 0);
    mx_pkt_cnt(&p, 0, 0);
    textured(&p, tp, MX_GS_PAGE(tex[i].page), tex[i].psm, tex[i].cbp);
    send(&p);
    CHECK_EQ(mx_gs_download(back, MX_GS_PAGE(tp), 1, MX_GS_PSM_CT32, 0, 0, 64, 32), 0);
    const u32 *px = (const u32 *)back;
    u32 bad = 0;
    for (u32 y = 0; y < 32; y++)
      for (u32 x = 0; x < 64; x++) {
        u32 w = i == 0 ? col8(pat8(x, y, 0)) : col4(i == 1 ? pat4l(x, y) : pat4h(x, y));
        if (px[y * 64 + x] != w) {
          if (!bad)
            mx_printf("%s (%u, %u): %08x, expected %08x\n", tex[i].name, x, y, px[y * 64 + x], w);
          bad++;
        }
      }
    mx_printf("hbyte: %s drawn, %u texels wrong\n", tex[i].name, bad);
    CHECK_EQ(bad, 0);
  }

  /* 5 */
  make_t8(1);
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, MX_GS_PAGE(cp), 1, MX_GS_PSM_T8H, 0, 0, 64, 32, t8);
  send(&p);
  CHECK_EQ(check_top(cp, pat8, 1), 0);
  CHECK_EQ(mx_gs_download(back, MX_GS_PAGE(cp), 1, MX_GS_PSM_CT24, 0, 0, 64, 32), 0);
  CHECK_EQ(back[(31 * 64 + 40) * 3], 0x11);
  CHECK_EQ(back[(31 * 64 + 40) * 3 + 2], 0x33);
  return t_done("hbyte");
}
