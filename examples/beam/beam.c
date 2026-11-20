/* Where the beam is when beam racing reckons it has left a strip
   (<mx/hires.h>, mx_hires_margin): a picture to read from the TV.

   The passes start before the beam has left their strip: early lines (40,
   fewer in low strips) before the line mx_hires_init reckons the strip's end
   at. A pass clears its strip to magenta, fills it a few more times (so
   that it stays magenta while the beam is in it), then draws it: black,
   with a scale. So each strip's last lines show magenta from the line the
   beam was at when the pass started; the lines above it show the scale.

   The scale's numbers are the lines the beam is later than reckoned: read
   the number at the top edge of each magenta band (the same at every band if
   the strips' lines are right but for one offset; the last band is the
   picture's end). 0 (the green line): the beam is where mx_hires_init
   reckons it; then a margin of a few lines is all a program needs. The
   clear takes a few lines to come down to the band, so the beam was that
   much higher when the pass started: the scale is drawn that much lower (by
   the fills' time, measured in the first seconds, and the mode's time a
   line).

   Pad: start exits. Arguments: m<mode> (ntsc pal 240p 288p 480p 576p 720p
   720p50 1080i 1080i50 1080p30 1080p25 1080p24; 1080i without), e<n> the
   lines early, s<n> seconds (120), d<n> the fills (0: as many as keep the
   strip magenta long enough, measured), t the scale as host:beam.tga at the
   end. The buffer is 640 wide (960 in the 1080 modes) and as high as the
   mode shows a field or frame.

   docs/beam.md: how to read it, what was measured, and where the rule is. */

#include <mx/crtc.h>
#include <mx/hires.h>
#include <mx/hud.h>
#include <mx/mx.h>
#include <mx/pad.h>
#include <mx/video.h>

/* core memory: the DMAC reads the records (an MXOS build is linked into VM) */
static u8 rec[3 * 128 * 1024] MX_IRQ_BSS MX_ALIGNED(64);

static u32 px(int x) { return mx_gs_px(0) + ((u32)x << 4); }
static u32 py(int y) { return mx_gs_py(0) + ((u32)y << 4); }

static void rect(int x0, int y0, int x1, int y1, u32 rgb) {
  u64 *c = mx_frame_direct(5);
  c = mx_e_ad_tag(c, 4, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(x0), py(y0), 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(x1), py(y1), 0)));
}

static u32 arg(const char *p) {
  u32 v = 0;
  for (; *p >= '0' && *p <= '9'; p++)
    v = v * 10 + (u32)(*p - '0');
  return v;
}

/* the passes' time a pass since the call before, us */
static u32 pass_us(void) {
  static u32 n0, s0;
  u32 n = 0, s = 0;
  for (u32 k = 0; k < mx_hires_passes(); k++)
    n += mx_hires_stats.pass_n[k], s += mx_hires_stats.pass_sum_us[k];
  u32 r = n != n0 ? (s - s0) / (n - n0) : 0;
  n0 = n, s0 = s;
  return r;
}

static const struct {
  const char *name, *shown;
  const struct mx_video_mode *m;
} modes[] = {{"1080i", "1080I", &mx_video_1080i},       {"ntsc", "NTSC", &mx_video_ntsc},
             {"pal", "PAL", &mx_video_pal},             {"240p", "240P", &mx_video_ntsc_240p},
             {"288p", "288P", &mx_video_pal_288p},      {"480p", "480P", &mx_video_480p},
             {"576p", "576P", &mx_video_576p},          {"720p", "720P", &mx_video_720p},
             {"720p50", "720P50", &mx_video_720p50},    {"1080i50", "1080I50", &mx_video_1080i50},
             {"1080p30", "1080P30", &mx_video_1080p30}, {"1080p25", "1080P25", &mx_video_1080p25},
             {"1080p24", "1080P24", &mx_video_1080p24}};

static int same(const char *a, const char *b) {
  while (*a && *a == *b)
    a++, b++;
  return *a == *b;
}

int main(int argc, char **argv) {
  int early = 0, secs = 120, tga = 0;
  u32 mode = 0;
  u32 fills = 0;
  for (int a = 1; a < argc; a++) {
    if (argv[a][0] == 'e')
      early = (int)arg(argv[a] + 1);
    if (argv[a][0] == 's')
      secs = (int)arg(argv[a] + 1);
    if (argv[a][0] == 'd')
      fills = arg(argv[a] + 1);
    if (argv[a][0] == 'm')
      for (u32 i = 0; i < sizeof modes / sizeof *modes; i++)
        if (same(argv[a] + 1, modes[i].name))
          mode = i;
    if (argv[a][0] == 't')
      tga = 1;
  }
  const struct mx_video_mode *vm = modes[mode].m;
  /* a buffer line a line of the field (interlaced: FRAME mode, as mx_hires_init sets) */
  int w = vm->lines == 1080 ? 960 : 640, H = vm->lines / (vm->interlace ? 2 : 1);
  /* the strips mx_hires_init makes of it: a third in whole pages, three at least */
  int sh = ((H + 2) / 3 + 63) / 64 * 64;
  while (sh > 64 && (H + sh - 1) / sh < 3)
    sh -= 64;
  if (!early)
    early = sh >= 192 ? 40 : sh / 5;
  /* the time of a line of the beam, us: the field's lines of the mode's timing */
  const struct mx_crtc *cr = vm->crtc;
  /* (its lines are half lines in the TV modes, 240p and 288p too, and when interlaced) */
  int half = cr->kind == MX_CRTC_NTSC || cr->kind == MX_CRTC_PAL || cr->interlace;
  float field = (float)(cr->vs + cr->vbpe + cr->vbp + cr->vdp + cr->vfpe + cr->vfp) / (half ? 2.0f : 1.0f);
  float line_us = 1e6f / ((float)vm->hz * field);
  /* magenta that long: past the strip's end with the beam 32 lines late, but
     done before the next strip's pass starts */
  float want = (float)(early + 32) * line_us, most = 0.8f * (float)sh * line_us;
  u32 target = (u32)(want < most ? want : most);
  mx_hires_own_z(1);
  mx_hires_margin(-early);
  if (mx_hires_init(vm, (u32)w, (u32)H, MX_GS_PSM_CT16S, rec, sizeof rec, (void *)0x70002000, 0x2000) < 0) {
    mx_printf("beam: no %s, or the framebuffer does not fit\n", modes[mode].name);
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("beam: %s, buffer %d x %d, %u strips of %u lines, %d lines early, a line %u us, magenta %u us\n",
            modes[mode].name, w, H, mx_hires_passes(), mx_hires_strip_height(), early, (u32)line_us, target);
  /* y in lines of the buffer (mx_hud's are of a screen of 224) */
  mx_hud_init(mx_gs.tbp, 224);
  int pad = mx_pad_init() >= 0;
  int np = (int)mx_hires_passes(), ts = H <= 288 ? 1 : 2; /* the text's scale */
  sh = (int)mx_hires_strip_height();
  /* the fills: none for two seconds, 16 for a second, then what takes the target */
  u32 base_us = 0, fill_us = 0, n = fills, phase = fills ? 3 : 0;
  u32 v0 = mx_gs.vsync, vp = v0, us = 0;
  for (;;) {
    if (pad && (mx_pad_pressed(0) & MX_PAD_START))
      break;
    if (mx_gs.vsync - v0 >= (u32)secs * mx_gs.hz)
      break;
    if (mx_gs.vsync - vp >= mx_gs.hz) {
      u32 t = pass_us();
      vp = mx_gs.vsync;
      if (phase == 0) {
        phase = 1; /* the first second: not counted */
      } else if (phase == 1) {
        base_us = t, n = 16, phase = 2;
      } else if (phase == 2) {
        fill_us = t > base_us + 16 ? (t - base_us) / 16 : 1;
        n = base_us < target ? (target - base_us) / fill_us : 0;
        n = n > 200 ? 200 : n;
        phase = 3;
      } else {
        us = t;
      }
    }
    u32 at = mx_gs.vsync;
    mx_frame_begin_n();
    mx_hires_clear(0xFF, 0, 0xFF);
    u64 *c = mx_frame_direct(2);
    c = mx_e_ad_tag(c, 1, 1);
    mx_frame_commit(mx_e_ad(c, MX_GS_REG_TEST_1, mx_gs_test(0, 0, 0, 0, 0, 0, 1, 1)));
    for (u32 i = 0; i < n; i++)
      rect(0, 0, w, H, 0xFF00FF);
    rect(0, 0, w, H, 0x000000);
    /* each strip's scale: the number e at the line the clear reaches as the
       beam does when the pass started with the beam at end - early - e (a
       fill takes cl lines of the beam for a line of the strip) */
    float cl = (float)fill_us * (float)np / (float)H / line_us;
    for (int k = 0; k < np; k++) {
      int y0 = k * sh, end = y0 + sh < H ? y0 + sh : H;
      for (int e = -early + 2; e <= 104; e += 2) {
        int y = (int)(((float)(end - early - e) - cl * (float)y0) / (1.0f - cl) + 0.5f);
        if (y >= end)
          continue;
        /* the text's lines at the top; a number's upper half */
        if (y < y0 + (k ? 4 * ts : 14 * ts))
          break;
        int x0 = w * 5 / 32, x1 = e % 16 == 0 ? w - x0 : e % 8 == 0 ? w / 2 : w * 9 / 32;
        rect(x0, y, e ? x1 : w, y + 1, e ? 0xFFFFFF : 0x00FF00);
        if (e % 8 == 0) {
          char str[8], *p = str;
          if (e < 0)
            *p++ = '-';
          mx_hud_u(p, (u32)(e < 0 ? -e : e));
          mx_hud_text((u32)(e % 16 == 0 ? w / 64 : w * 5 / 64), (u32)(y - 3 * ts), str, e ? 0xFFFFFF : 0x00FF00,
                      (u32)ts);
        }
      }
    }
    char str[96];
    char *p = mx_hud_cat(str, "BEAM ");
    p = mx_hud_cat(p, modes[mode].shown);
    p = mx_hud_cat(p, "  ");
    p = mx_hud_u(p, (u32)early);
    p = mx_hud_cat(p, " EARLY  FILLS ");
    p = mx_hud_u(p, n);
    p = mx_hud_cat(p, " OF ");
    p = mx_hud_u(p, fill_us);
    p = mx_hud_cat(p, " US  PASS ");
    p = mx_hud_u(p, us);
    p = mx_hud_cat(p, "  LATE ");
    mx_hud_u(p, mx_hires_stats.late + mx_hires_stats.over);
    mx_hud_text((u32)(w / 64), (u32)(2 * ts), str, 0xFFFFFF, (u32)ts);
    mx_frame_end();
    while (mx_gs.vsync - at < 6)
      ;
  }
  mx_printf("beam: %s, %d lines early, %u fills of %u us and %u us without; a pass %u us; fields %u, passes %u, late %u, "
            "over %u\n", modes[mode].name, early, n, fill_us, base_us, us, mx_hires_stats.fields, mx_hires_stats.passes,
            mx_hires_stats.late, mx_hires_stats.over);
  if (tga)
    mx_printf("beam: host:beam.tga %d\n", mx_hires_save_tga("host:beam.tga"));
  MX_TEST_END(0);
  return 0;
}
