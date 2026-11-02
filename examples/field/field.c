/* Field rendering, to look at on a TV: which field a half-line shift
   belongs to (docs/gif-gs.md, Display).

   In interlaced FRAME mode a 224-line buffer is shown by both fields, the
   second field's raster lines half a buffer line below the first's. Drawing
   each frame for the field that will show it, with the scene sampled half a
   line lower for the lower field (XYOFFSET.Y + 8 in 12.4), gives the two
   fields of a 448-line picture at the drawing cost of 224 lines. Which field
   is the lower one is what this example finds out: the right setting looks
   like the 448-line reference, the wrong one worse than no shift (each field
   two raster lines off instead of one).

   Settings (NTSC 640 x 224 / 448, PAL 640 x 256 / 512):

     NO SHIFT   224-line buffer, both fields the same picture
     SHIFT F1   224 lines, frames for field 1 (CSR.FIELD) sampled half a line lower
     SHIFT F0   the same for field 0
     448 FIELD  the reference: a 448-line buffer, FIELD mode, every frame drawn in full

   The scene: a static wedge with a shallow edge, a slowly turning square, a
   bar moving up and down by 1/8 line a frame, and lines one line (of 224)
   high (these twitter in every setting: a hard horizontal edge does on an
   interlaced screen). The text is drawn without the shift.

   Each frame is drawn right after a vblank, for the field that the next
   vblank starts (mx_video_field), and must be shown there: WRONG counts
   frames shown on the other field, LATE frames not shown at the next vblank.

   Pad: left/right the setting, triangle NTSC/PAL, start exits. Until a
   button is pressed every setting runs a few seconds (NTSC, then PAL), then
   the program exits. Arguments: s<n> n seconds a setting, p start with PAL. */

#include <mx/hud.h>
#include <mx/mx.h>
#include <mx/pad.h>

struct setting {
  const char *name;
  int full;  /* a 448/512-line buffer in FIELD mode */
  int shift; /* the field drawn half a line lower, -1: none */
};

static const struct setting settings[] = {
    {"NO SHIFT", 0, -1},
    {"SHIFT F1", 0, 1},
    {"SHIFT F0", 0, 0},
    {"448 FIELD", 1, -1},
};
#define NSETTINGS (int)(sizeof settings / sizeof *settings)

static u8 ring[64 * 1024] MX_ALIGNED(64 * 1024);
static int cur, pal, running;
static u32 first, wrong, late, scale;

/* The scene is in lines of a 224-line screen (PAL: 256): scale (1, 2)
   takes them to the buffer. ox, oy: the target's XYOFFSET. */
static u32 ox, oy;
static u32 px(float x) { return ox + (u32)(x * 16.0f); }
static u32 py(float y) { return oy + (u32)(y * (float)scale * 16.0f); }

static void tri(float x0, float y0, float x1, float y1, float x2, float y2, u32 rgb) {
  u64 *c = mx_frame_direct(6);
  c = mx_e_ad_tag(c, 5, 1);
  c = mx_e_ad(c, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_TRIANGLE, 0, 0, 0, 0, 0, 0, 0, 0));
  c = mx_e_ad(c, MX_GS_REG_RGBAQ, mx_gs_rgbaq(rgb >> 16, rgb >> 8 & 0xFF, rgb & 0xFF, 0x80, 1.0f));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(x0), py(y0), 0));
  c = mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(x1), py(y1), 0));
  mx_frame_commit(mx_e_ad(c, MX_GS_REG_XYZ2, mx_gs_xyz(px(x2), py(y2), 0)));
}

static void rect(float x0, float y0, float x1, float y1, u32 rgb) {
  tri(x0, y0, x1, y0, x0, y1, rgb);
  tri(x1, y0, x1, y1, x0, y1, rgb);
}

static void scene(u32 k, float c, float s) {
  u32 lines = pal ? 256 : 224;
  /* a wedge: its lower edge falls 30 lines over 560 pixels */
  tri(40, 24, 600, 24, 40, 54, 0xFFFFFF);
  /* a square turning about (480, lines / 2) */
  float cx = 480, cy = (float)lines / 2, r = 50;
  float ax = r * c, ay = r * s;
  tri(cx + ax, cy + ay, cx - ay, cy + ax, cx - ax, cy - ay, 0xFFC040);
  tri(cx - ax, cy - ay, cx + ay, cy - ax, cx + ax, cy + ay, 0xFFC040);
  /* a bar going up and down by 1/8 line a frame over 64 lines */
  u32 t = k % 1024;
  float y = 80 + (float)(t < 512 ? t : 1024 - t) / 8.0f;
  rect(40, y, 260, y + 3, 0x40FF40);
  /* lines one line high, two apart */
  for (u32 i = 0; i < 6; i++) {
    float l = (float)(lines - 60 + 3 * i);
    rect(300, l, 400, l + 1, 0xFFFFFF);
  }
}

static void wait_shown(void) {
  u32 t0 = mx_count();
  while (mx_gs.shown < mx_gs.frames && mx_count() - t0 < 300000000u)
    ;
}

/* the end of a setting: its counts */
static void stop(void) {
  if (!running)
    return;
  wait_shown();
  mx_printf("field: %s %s: %u frames, wrong field %u, late %u\n", pal ? "PAL" : "NTSC",
            settings[cur].name, mx_gs.frames - first, wrong, late);
  mx_frame_stop();
  running = 0;
}

static void setup(int i) {
  const struct setting *st = &settings[i];
  stop();
  cur = i;
  scale = st->full ? 2 : 1;
  u32 h = (pal ? 256 : 224) * scale;
  /* up to 256 lines FRAME mode, more FIELD mode */
  mx_gs_init_mode(640, h, pal ? MX_GS_PAL : 0);
  mx_frame_init(ring, sizeof ring, (void *)0x70002000, 0x2000);
  mx_hud_init(mx_gs.tbp, h);
  running = 1;
  first = mx_gs.frames;
  wrong = late = 0;
}

int main(int argc, char **argv) {
  int secs = 0;
  for (int a = 1; a < argc; a++) {
    if (argv[a][0] == 'p')
      pal = 1;
    if (argv[a][0] == 's')
      for (const char *p = argv[a] + 1; *p >= '0' && *p <= '9'; p++)
        secs = secs * 10 + (*p - '0');
  }
  u32 dwell = 60 * (u32)(secs ? secs : 3);
  setup(0);
  int pad = mx_pad_init() >= 0, manual = 0, start_pal = pal;
  u32 step = 0, at = mx_gs.frames;
  float c = 1, s = 0;
  const float d = 0.005f; /* radians a frame */
  u8 par[4] = {0};        /* the field each frame is drawn for */
  for (;;) {
    u32 b = pad ? mx_pad_pressed(0) : 0;
    if (b & MX_PAD_START)
      break;
    manual |= b != 0;
    if (!manual && mx_gs.frames - at >= dwell) {
      if (++step == 2 * NSETTINGS)
        break;
      stop();
      pal = start_pal ^ (step >= NSETTINGS);
      setup((int)(step % NSETTINGS));
      at = mx_gs.frames;
    }
    if (b & (MX_PAD_LEFT | MX_PAD_RIGHT))
      setup((cur + (b & MX_PAD_RIGHT ? 1 : NSETTINGS - 1)) % NSETTINGS);
    if (b & MX_PAD_TRIANGLE) {
      stop();
      pal ^= 1;
      setup(cur);
    }

    /* right after a vblank: frame k - 1 was shown at it (frame k waits for
       that), and frame k, drawn within this field, is shown at the next */
    mx_gs_wait_vsync();
    int buf = mx_frame_begin(0x10, 0x18, 0x30);
    u32 k = mx_gs.frames, f = mx_video_field();
    if (k > first) {
      if (mx_gs.shown != k)
        late++;
      else if (par[(k - 1) & 3] != f)
        wrong++;
    }
    u32 next = f ^ 1;
    par[k & 3] = (u8)next;

    /* the scene, half a line lower for the shifted field */
    struct mx_rt t = mx_gs.rt[buf];
    if (settings[cur].shift == (int)next)
      t.oy += 8;
    mx_frame_target(&t, 0);
    ox = t.ox, oy = t.oy;
    scene(k, c, s);

    /* the text, not shifted */
    mx_frame_target(&mx_gs.rt[buf], 0);
    char str[96];
    char *p = mx_hud_cat(str, pal ? "PAL  " : "NTSC  ");
    mx_hud_cat(p, settings[cur].name);
    mx_hud_text(40, 186, str, 0xFFFFFF, 2);
    p = mx_hud_cat(str, "WRONG FIELD ");
    p = mx_hud_u(p, wrong);
    p = mx_hud_cat(p, "  LATE ");
    mx_hud_u(p, late);
    mx_hud_text(40, 206, str, 0xC0C0C0, 1);
    mx_frame_end();

    float c2 = c - s * d, s2 = s + c * d;
    float n = 1.5f - 0.5f * (c2 * c2 + s2 * s2);
    c = c2 * n;
    s = s2 * n;
  }
  stop();
  mx_printf("field: done\n");
  MX_TEST_END(0);
  return 0;
}
