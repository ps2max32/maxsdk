/* High resolution by passes (<mx/hires.h>): 1920 x 1080 interlaced from
   one 1920 x 540 16-bit framebuffer, drawn in three strips just ahead of
   the beam, both fields (the lower one half a line lower).

   The scene, in lines of the 540-line buffer: a turning square, two
   triangles through each other (Z: the overlap shows which is in front
   where), a wedge with a shallow edge, lines one line high, and the
   counters (fields, passes, late passes, records shown).

   The frames come at a quarter of the field rate (each record is played
   four fields): the picture stays whole, as every field draws all three
   strips from the newest record.

   Pad: start exits. Without it the program runs a few seconds, saves both
   fields as host:hires.tga (1920 x 1080) and exits. Arguments: s<n> n
   seconds, f<n> frames every n fields (default 4). */

#include <mx/hires.h>
#include <mx/hud.h>
#include <mx/mx.h>
#include <mx/pad.h>

#define W 1920
#define H 540

static u8 rec[3 * 128 * 1024] MX_ALIGNED(64);

static u32 px(float x) { return mx_gs_px(0) + (u32)(x * 16.0f); }
static u32 py(float y) { return mx_gs_py(0) + (u32)(y * 16.0f); }

/* A flat triangle; ctx 0 writes Z (z: larger is nearer, ZTST GEQUAL) */
static void tri(const float *v, u32 rgb, u32 z0, u32 z1, u32 z2) {
  u64 *c = mx_frame_direct(6);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(v[0]), py(v[1]), z0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(v[2]), py(v[3]), z1));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(v[4]), py(v[5]), z2)));
}

static void rect(float x0, float y0, float x1, float y1, u32 rgb) {
  float a[6] = {x0, y0, x1, y0, x0, y1}, b[6] = {x1, y0, x1, y1, x0, y1};
  tri(a, rgb, 0, 0, 0);
  tri(b, rgb, 0, 0, 0);
}

static void scene(u32 k, float c, float s) {
  /* Z test GEQUAL */
  u64 *e = mx_frame_direct(2);
  e = mx_e_ad_tag(e, 1, 1);
  mx_frame_commit(mx_e_ad(e, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 2)));
  /* a wedge: its lower edge falls 40 lines over 1600 pixels */
  float wv[6] = {160, 40, 1760, 40, 160, 80};
  tri(wv, 0xFFFFFF, 0, 0, 0);
  /* a square turning about (1440, 270) */
  float cx = 1440, cy = 270, r = 150, ax = r * c, ay = r * s;
  /* a buffer line is two lines of the screen: y halved */
  float q0[6] = {cx + ax, cy + ay / 2, cx - ay, cy + ax / 2, cx - ax, cy - ay / 2};
  float q1[6] = {cx - ax, cy - ay / 2, cx + ay, cy - ax / 2, cx + ax, cy + ay / 2};
  tri(q0, 0xFFC040, 0, 0, 0);
  tri(q1, 0xFFC040, 0, 0, 0);
  /* two triangles through each other: red nearer on the left, blue on the
     right; moving up and down so the strips' edges cross them */
  float dy = (float)(k % 240);
  dy = dy < 120 ? dy : 240 - dy;
  float t0[6] = {200, 150 + dy, 900, 200 + dy, 200, 400 + dy};
  float t1[6] = {900, 150 + dy, 900, 400 + dy, 200, 260 + dy};
  tri(t0, 0xFF4040, 60000, 0, 60000);
  tri(t1, 0x4060FF, 0, 60000, 30000);
  /* lines one line high, two apart */
  for (u32 i = 0; i < 8; i++)
    rect(1000, (float)(440 + 3 * i), 1300, (float)(441 + 3 * i), 0xFFFFFF);
}

int main(int argc, char **argv) {
  int secs = 4;
  u32 every = 4;
  for (int a = 1; a < argc; a++) {
    if (argv[a][0] == 's' || argv[a][0] == 'f') {
      u32 v = 0;
      for (const char *p = argv[a] + 1; *p >= '0' && *p <= '9'; p++)
        v = v * 10 + (u32)(*p - '0');
      if (argv[a][0] == 's')
        secs = (int)v;
      else
        every = v ? v : 1;
    }
  }
  if (mx_hires_init(&mx_video_1080i, W, H, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000,
                    0x2000) < 0) {
    mx_printf("hires: no 1080i, or the framebuffer does not fit\n");
    MX_TEST_END(1);
  }
  mx_hud_init(mx_gs.tbp, H);
  int pad = mx_pad_init() >= 0;
  float c = 1, s = 0;
  const float d = 0.02f;
  u32 v0 = mx_gs.vsync, k = 0;
  for (;;) {
    if (pad && (mx_pad_pressed(0) & MX_PAD_START))
      break;
    if (mx_gs.vsync - v0 >= (u32)secs * mx_gs.hz)
      break;
    /* a frame every few fields */
    u32 at = mx_gs.vsync;
    mx_frame_begin_n();
    mx_hires_clear(0x10, 0x18, 0x30);
    scene(k, c, s);
    char str[96];
    char *p = mx_hud_cat(str, "1080I HIRES  FIELDS ");
    p = mx_hud_u(p, mx_hires_stats.fields);
    p = mx_hud_cat(p, "  PASSES ");
    p = mx_hud_u(p, mx_hires_stats.passes);
    p = mx_hud_cat(p, "  LATE ");
    p = mx_hud_u(p, mx_hires_stats.late);
    p = mx_hud_cat(p, "  SHOWN ");
    mx_hud_u(p, mx_hires_stats.shown);
    mx_hud_text(160, 200, str, 0xFFFFFF, 3);
    mx_frame_end();
    k++;
    float c2 = c - s * d, s2 = s + c * d, n = 1.5f - 0.5f * (c2 * c2 + s2 * s2);
    c = c2 * n, s = s2 * n;
    while (mx_gs.vsync - at < every)
      ;
  }
  mx_printf("hires: %u frames, fields %u, passes %u, late %u, shown %u, record max %u bytes\n", k,
            mx_hires_stats.fields, mx_hires_stats.passes, mx_hires_stats.late,
            mx_hires_stats.shown, mx_hires_stats.rec_max);
  int r = mx_hires_save_tga("host:hires.tga");
  mx_printf("hires: screenshot %d\n", r);
  int ok = r == 0 && mx_hires_stats.shown > 0 && mx_hires_stats.fields > 0;
  MX_TEST_END(ok ? 0 : 1);
  return 0;
}
