/* What the GS's scissor saves (PS2 only): beam racing draws a frame once per
   strip, each pass with the scissor on its strip, so a large triangle is
   sent in every pass and drawn in a part of it.

   N pairs of triangles that cover the screen (640 x 448, Z test GEQUAL with
   Z writes, each pair nearer than the one before: every pixel is written),
   flat and textured (bilinear, a 64 x 64 texture), with the scissor on the
   whole screen, on a part of its lines, on a part of its columns, and with
   triangles that reach past the screen into the guard band (as those near
   the camera do): EE cycles from the DMA's start to FINISH, and per pixel
   inside the scissor. If the pixels a scissor leaves out cost nothing, the
   cycles a pixel inside it are the same in every case. */

#include <mx/mx.h>

#define REPS 3
#define N 32
#define W 640
#define H 448

static u64 pk[4096] MX_CORE_BSS MX_ALIGNED(64);
static u64 env[128] MX_CORE_BSS MX_ALIGNED(64);
static u32 tex[64 * 64] MX_CORE_BSS MX_ALIGNED(64);
static u32 up[256] MX_CORE_BSS MX_ALIGNED(64);

static u32 median(u32 *v, int n) {
  for (int i = 1; i < n; i++)
    for (int j = i; j > 0 && v[j] < v[j - 1]; j--) {
      u32 t = v[j];
      v[j] = v[j - 1];
      v[j - 1] = t;
    }
  return v[n / 2];
}

static void setup(u32 x0, u32 x1, u32 y0, u32 y1) {
  u64 *c = env, *t = c;
  c = mx_e_cnt(c, 0, 0, 0);
  c = mx_gs_e_draw_env(c, 0, 0);
  c = mx_gs_e_clear(c, 0, 0, 0, 0);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2));
  c = mx_e_ad(c, MX_GS_REG_TEX0_1, mx_gs_tex0(mx_gs.tbp, 1, MX_GS_PSM_CT32, 6, 6, 1, 0));
  c = mx_e_ad(c, MX_GS_REG_TEX1_1, mx_gs_tex1(0, 0, 1, 1, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_SCISSOR_1, mx_gs_scissor(x0, x1, y0, y1));
  c = mx_e_ad(c, MX_GS_REG_FINISH, 0);
  mx_e_close(t, c);
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(env, sizeof env);
  u32 f0 = mx_gs.finish;
  mx_gs_send(env);
  mx_gs_wait_finish(f0, 1);
}

/* grow: the triangles reach that many pixels past the screen on every side
   (into the GS's guard band; the scissor is the screen or less) */
static u32 run(int textured, u32 x0, u32 x1, u32 y0, u32 y1, s32 grow) {
  u64 *c = pk, *t = c;
  c = mx_e_cnt(c, 0, 0, 0);
  /* a strip of four vertices a pair: two triangles */
  for (u32 i = 0; i < N; i++) {
    c = mx_e_giftag(c, 4, 0, (s32)mx_gs_prim(MX_GS_PRIM_TRISTRIP, 0, textured, 0, 0, 0, 0, 0, 0), MX_GIF_FLG_PACKED,
                    3, 0x512);
    for (u32 v = 0; v < 4; v++) {
      s32 x = v & 1 ? W + grow : -grow, y = v & 2 ? H + grow : -grow;
      c = mx_e_st(c, (float)(x / 64), (float)(y / 64), 1.0f);
      c = mx_e_rgbaq(c, 0x80, 0x60 + i, 0x40, 0x80);
      c = mx_e_xyz2(c, (u32)((s32)mx_gs_px(0) + x * 16), (u32)((s32)mx_gs_py(0) + y * 16), 1000 + i * 100, 0);
    }
  }
  c = mx_e_ad_tag(c, 1, 1);
  c = mx_e_ad(c, MX_GS_REG_FINISH, 0);
  mx_e_close(t, c);
  c = mx_e_end(c, 0, 0, 0);
  mx_dcache_writeback(pk, (u32)((u8 *)c - (u8 *)pk));
  u32 v[REPS];
  for (int r = 0; r < REPS; r++) {
    setup(x0, x1, y0, y1);
    u32 f0 = mx_gs.finish, t0 = mx_count();
    mx_dma_chain(MX_CH_GIF, pk, 0);
    mx_gs_wait_finish(f0, 1);
    v[r] = mx_count() - t0;
  }
  return median(v, REPS);
}

int main(void) {
  mx_gs_init(W, H);
  for (u32 i = 0; i < 64 * 64; i++)
    tex[i] = 0x80000000u | (i * 0x010305u & 0xFFFFFFu);
  mx_dcache_writeback(tex, sizeof tex);
  {
    struct mx_pkt p;
    mx_pkt_begin(&p, up, sizeof up, 0);
    mx_pkt_cnt(&p, 0, 0);
    mx_gs_upload(&p, mx_gs.tbp, 1, MX_GS_PSM_CT32, 0, 0, 64, 64, tex);
    mx_gs_finish_ad(&p);
    mx_pkt_end(&p, 0, 0);
    mx_pkt_finish(&p);
    mx_pkt_writeback(&p);
    u32 f0 = mx_gs.finish;
    mx_gs_send(up);
    mx_gs_wait_finish(f0, 1);
  }
  static const struct {
    const char *name;
    u32 x0, x1, y0, y1;
    s32 grow;
  } sc[] = {{"all of the screen", 0, W - 1, 0, H - 1, 0},
            {"the top third of the lines", 0, W - 1, 0, 148, 0},
            {"the middle third of the lines", 0, W - 1, 149, 297, 0},
            {"a ninth of the lines (200-249)", 0, W - 1, 200, 249, 0},
            {"one line (224)", 0, W - 1, 224, 224, 0},
            {"the left third of the columns", 0, 212, 0, H - 1, 0},
            {"the middle third of the columns", 213, 425, 0, H - 1, 0},
            {"one column (320)", 320, 320, 0, H - 1, 0},
            {"all of the screen, the triangles 512 pixels past it each way", 0, W - 1, 0, H - 1, 512},
            {"all of the screen, the triangles 1536 pixels past it each way", 0, W - 1, 0, H - 1, 1536},
            {"the middle third of the lines, the triangles 1536 pixels past the screen", 0, W - 1, 149, 297, 1536}};
#define NSC (sizeof sc / sizeof sc[0])
  u32 px = N * W * H;
  mx_printf("p41_scissor: %u pairs of triangles over %u x %u (%u pixels), Z GEQUAL with Z writes\n", N, W, H, px);
  mx_debug_hold(1);
  static u32 res[2][NSC];
  for (int t = 0; t < 2; t++)
    for (u32 s = 0; s < NSC; s++)
      res[t][s] = run(t, sc[s].x0, sc[s].x1, sc[s].y0, sc[s].y1, sc[s].grow);
  mx_debug_hold(0);
  for (int t = 0; t < 2; t++)
    for (u32 s = 0; s < NSC; s++)
      mx_printf("p41_scissor: %s, scissor on %s: %u cycles, %u %% of the first; %u.%03u a pixel inside the scissor\n",
                t ? "textured" : "flat", sc[s].name, res[t][s], (u32)((u64)res[t][s] * 100 / res[t][0]),
                res[t][s] / (N * (sc[s].x1 - sc[s].x0 + 1) * (sc[s].y1 - sc[s].y0 + 1)),
                (u32)((u64)res[t][s] * 1000 / (N * (sc[s].x1 - sc[s].x0 + 1) * (sc[s].y1 - sc[s].y0 + 1)) % 1000));
  MX_TEST_END(0);
  return 0;
}
