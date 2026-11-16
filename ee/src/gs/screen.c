/* <mx/screen.h>: the screens of the system's video settings. Plain
   arithmetic over the modes' areas, for programs (libmx, libmxk) and the
   kernel (its console, the same rules). */

#include <mx/rt.h>
#include <mx/screen.h>
#include <mx/setting.h>
#include <mx/string.h>

/* Each mode: its timing (<mx/video.h>), and its area with overscan on
   (the part every TV shows) and off (all of the 704 x 480/576 of BT.601;
   the area the TV's shape is of). The areas are centred on the mode's own
   (dx, dy as there, moved by half the difference). */
struct area {
  const struct mx_video_mode *m;
  u16 vck_on, lines_on, vck_off, lines_off;
};
static const struct area areas[MX_SCREEN_MODES] = {
    [MX_SCREEN_480I] = {&mx_video_ntsc, 2560, 448, 2816, 480},
    [MX_SCREEN_576I] = {&mx_video_pal, 2560, 512, 2816, 576},
    [MX_SCREEN_480P] = {&mx_video_480p, 1280, 448, 1408, 480},
    [MX_SCREEN_576P] = {&mx_video_576p, 1280, 512, 1408, 576},
    [MX_SCREEN_720P] = {&mx_video_720p, 1280, 720, 1280, 720},
    [MX_SCREEN_1080I] = {&mx_video_1080i, 1920, 1080, 1920, 1080},
    [MX_SCREEN_240P] = {&mx_video_ntsc_240p, 2560, 224, 2816, 240},
    [MX_SCREEN_288P] = {&mx_video_pal_288p, 2560, 256, 2816, 288},
    [MX_SCREEN_720P50] = {&mx_video_720p50, 1280, 720, 1280, 720},
    [MX_SCREEN_1080I50] = {&mx_video_1080i50, 1920, 1080, 1920, 1080},
    [MX_SCREEN_1080P24] = {&mx_video_1080p24, 1920, 1080, 1920, 1080},
    [MX_SCREEN_1080P25] = {&mx_video_1080p25, 1920, 1080, 1920, 1080},
    [MX_SCREEN_1080P30] = {&mx_video_1080p30, 1920, 1080, 1920, 1080},
};
static const char *const names[MX_SCREEN_MODES] = {
    [MX_SCREEN_480I] = "480i",       [MX_SCREEN_576I] = "576i",       [MX_SCREEN_480P] = "480p",
    [MX_SCREEN_576P] = "576p",       [MX_SCREEN_720P] = "720p",       [MX_SCREEN_1080I] = "1080i",
    [MX_SCREEN_240P] = "240p",       [MX_SCREEN_288P] = "288p",       [MX_SCREEN_720P50] = "720p50",
    [MX_SCREEN_1080I50] = "1080i50", [MX_SCREEN_1080P24] = "1080p24", [MX_SCREEN_1080P25] = "1080p25",
    [MX_SCREEN_1080P30] = "1080p30",
};

const char *mx_screen_name(u32 mode) { return mode && mode < MX_SCREEN_MODES ? names[mode] : ""; }

u32 mx_screen_mode_of(const char *name) {
  for (u32 i = 1; i < MX_SCREEN_MODES; i++)
    if (!strcmp(name, names[i]))
      return i;
  return 0;
}

/* the TV timings (RGB possible); the 16:9 ones (720 and 1080 lines) */
#define SD_MODES                                                                                   \
  (1u << MX_SCREEN_480I | 1u << MX_SCREEN_576I | 1u << MX_SCREEN_240P | 1u << MX_SCREEN_288P)
static int sd(u32 mode) { return SD_MODES >> mode & 1; }
static int hd(u32 mode) { return areas[mode].lines_on >= 720; }

void mx_screen_conf_from(struct mx_screen_conf *c,
                         int (*get)(const char *key, char *value, u32 size)) {
  char v[MX_SETTING_VALUE];
  memset(c, 0, sizeof *c);
  c->pal = mx_video_default() == &mx_video_pal;
  u32 region = c->pal ? MX_SCREEN_576I : MX_SCREEN_480I;
#define IS(key, val) (get && get(key, v, sizeof v) > 0 && !strcmp(v, val))
  c->rgb = IS("video.out", "rgb");
  c->native = IS("video.scale", "native");
  c->wide = IS("video.aspect", "16:9");
  c->full = IS("video.overscan", "off");
#undef IS
  c->mode = (u8)(get && get("video.mode", v, sizeof v) > 0 ? mx_screen_mode_of(v) : 0);
  if (!c->mode || (c->rgb && !sd(c->mode)))
    c->mode = (u8)region;
  u32 allowed = 1u << region | 1u << c->mode;
  if (get && get("video.allowed", v, sizeof v) > 0) {
    /* names apart by spaces or commas */
    for (char *p = v; *p;) {
      while (*p == ' ' || *p == ',')
        p++;
      char *e = p;
      while (*e && *e != ' ' && *e != ',')
        e++;
      char k = *e;
      *e = 0;
      allowed |= 1u << mx_screen_mode_of(p);
      *e = k;
      p = e;
    }
  }
  allowed &= ~1u; /* 0: a name not known */
  if (c->rgb)
    allowed &= SD_MODES;
  c->allowed = (u16)allowed;
}

void mx_screen_conf(struct mx_screen_conf *c) { mx_screen_conf_from(c, mx_setting_get); }

u32 mx_screen_modes(const struct mx_screen_conf *c) {
  return c->native ? c->allowed : 1u << c->mode;
}

/* the mode's area (overscan) and that of the TV's shape */
static void area_of(const struct mx_screen_conf *c, u32 mode, u32 *vck, u32 *lines) {
  const struct area *a = &areas[mode];
  *vck = c->full ? a->vck_off : a->vck_on;
  *lines = c->full ? a->lines_off : a->lines_on;
}
static u32 tv_aspect(const struct mx_screen_conf *c, u32 mode) {
  return hd(mode) || c->wide ? 1778 : 1333;
}
static u32 interlaced(u32 mode) { return areas[mode].m->interlace; }

/* the shape of vck x lines on the TV, x 1000 */
static u32 shape(const struct mx_screen_conf *c, u32 mode, u32 vck, u32 lines) {
  const struct area *a = &areas[mode];
  return (u32)((u64)tv_aspect(c, mode) * vck * a->lines_off / ((u64)a->vck_off * lines));
}

/* s's other fields from mode, w, h, ffmd, magh, magv */
static void finish(const struct mx_screen_conf *c, u32 mode, struct mx_screen *s) {
  u32 vck, lines;
  area_of(c, mode, &vck, &lines);
  u32 ll = interlaced(mode) && s->ffmd ? 2 : 1;
  u32 cv = s->w * s->magh, cl = s->h * s->magv * ll;
  s->mode = (u8)mode;
  s->hz = areas[mode].m->hz;
  s->aspect = (u16)shape(c, mode, cv, cl);
  s->cover = (u16)((u64)cv * cl * 1000 / ((u64)vck * lines));
  s->flags = 0;
  if (mode == c->mode)
    s->flags |= MX_SCREEN_F_SHELL;
  if (cv * 100 >= vck * 99 && cl * 100 >= lines * 99)
    s->flags |= MX_SCREEN_F_FILL;
  if (2 * mx_rt_pages(s->w, s->h, MX_GS_PSM_CT32) + mx_rt_pages(s->w, s->h, MX_GS_PSM_Z16) <= 512)
    s->flags |= MX_SCREEN_F_DOUBLE;
}

/* fits GS memory at all: one 16-bit buffer */
static int fits(u32 w, u32 h) {
  return w >= 256 && w <= 2048 && h >= 128 && mx_rt_pages(w, h, MX_GS_PSM_CT16) <= 512;
}

/* height choice hv (0-7) of a mode: FRAME (or progressive) magv 1-4, then
   FIELD magv 1-4; 0 if there is no such one */
static int height(u32 mode, u32 lines, u32 hv, u32 *ffmd, u32 *magv, u32 *h) {
  u32 v = hv % 4 + 1;
  if (!interlaced(mode)) {
    if (hv >= 4)
      return 0;
    *ffmd = 0, *magv = v, *h = lines / v;
    return 1;
  }
  if (hv < 4) {
    *ffmd = 1, *magv = v, *h = lines / 2 / v;
    return 1;
  }
  *ffmd = 0, *magv = v, *h = lines / v;
  /* the same height in FRAME mode: that one (no flicker) */
  for (u32 k = 1; k <= 4; k++)
    if (lines / 2 / k == *h)
      return 0;
  return 1;
}

int mx_screen_fills(const struct mx_screen_conf *c, u32 mode, struct mx_screen *s, int max) {
  if (!mode || mode >= MX_SCREEN_MODES)
    return 0;
  u32 vck, lines;
  area_of(c, mode, &vck, &lines);
  int n = 0;
  /* the heights: progressive lines / magv; interlaced FRAME lines / 2 /
     magv, or FIELD lines / magv where FRAME has not that height (all lines;
     1080i's 360) */
  for (u32 hv = 0; hv < 8; hv++) {
    u32 ffmd, magv, h;
    if (!height(mode, lines, hv, &ffmd, &magv, &h))
      continue;
    for (u32 magh = 1; magh <= 16; magh++) {
      u32 w = vck / magh;
      if (!fits(w, h))
        continue;
      if (n < max) {
        struct mx_screen *t = &s[n];
        memset(t, 0, sizeof *t);
        t->w = (u16)w, t->h = (u16)h, t->ffmd = (u8)ffmd;
        t->magh = (u8)magh, t->magv = (u8)magv;
        finish(c, mode, t);
      }
      n++;
    }
  }
  /* the largest first */
  int m = n < max ? n : max;
  for (int i = 1; i < m; i++)
    for (int j = i; j > 0 && (u32)s[j].w * s[j].h > (u32)s[j - 1].w * s[j - 1].h; j--) {
      struct mx_screen t = s[j];
      s[j] = s[j - 1];
      s[j - 1] = t;
    }
  return n;
}

/* how far a is from b, as a fraction x 1000 of b */
static u32 off(u32 a, u32 b) { return b ? (a > b ? a - b : b - a) * 1000 / b : 0; }

/* the score of a screen (lower is better): its shape against aspect, three
   times as much as the area it leaves black */
static u32 score(const struct mx_screen *s, u32 aspect) {
  return 3 * off(s->aspect, aspect) + (1000 - (s->cover > 1000 ? 1000 : s->cover));
}

int mx_screen_fit(const struct mx_screen_conf *c, u32 mode, u32 w, u32 h, u32 aspect,
                  struct mx_screen *s) {
  if (!mode || mode >= MX_SCREEN_MODES || !w || !h)
    return -1;
  u32 vck, lines;
  area_of(c, mode, &vck, &lines);
  int found = 0;
  u32 best = ~0u;
  for (u32 magh = 1; magh <= 16 && w * magh <= vck; magh++)
    for (u32 hv = 0; hv < 8; hv++) {
      /* FRAME (progressive) magv 1-4, then FIELD magv 1-4; FIELD only for
         what FRAME cannot show (more lines than a field, or a magv FRAME
         has not: 1080i's 3) */
      u32 v = hv % 4 + 1, ffmd = 0, magv = v;
      if (!interlaced(mode)) {
        if (hv >= 4 || h * v > lines)
          continue;
      } else if (hv < 4) {
        ffmd = 1;
        if (h * 2 * v > lines)
          continue;
      } else {
        if (h * v > lines || (v % 2 == 0 && h * v <= lines))
          continue; /* an even FIELD magv is FRAME's magv / 2 */
      }
      struct mx_screen t;
      memset(&t, 0, sizeof t);
      t.w = (u16)w, t.h = (u16)h, t.ffmd = (u8)ffmd, t.magh = (u8)magh, t.magv = (u8)magv;
      finish(c, mode, &t);
      u32 sc = score(&t, aspect ? aspect : t.aspect);
      if (sc < best)
        best = sc, *s = t, found = 1;
    }
  return found ? 0 : -1;
}

/* how much a and b differ, as their ratio - 1, x 1000 */
static u32 ratio(u32 a, u32 b) {
  if (!a || !b)
    return 1000000;
  return a > b ? (a - b) * 1000 / b : (b - a) * 1000 / a;
}

/* not the shell's mode: chosen only when clearly better */
#define OTHER_MODE 150

int mx_screen_pick(const struct mx_screen_conf *c, u32 w, u32 h, u32 aspect, u32 flags,
                   struct mx_screen *s) {
  u32 modes = mx_screen_modes(c), best = ~0u;
  int found = 0;
  for (u32 mode = 1; mode < MX_SCREEN_MODES; mode++) {
    if (!(modes >> mode & 1))
      continue;
    u32 extra = mode == c->mode ? 0 : OTHER_MODE;
    struct mx_screen t;
    if (flags & MX_SCREEN_EXACT) {
      if (mx_screen_fit(c, mode, w, h, aspect, &t))
        continue;
      u32 sc = score(&t, aspect ? aspect : t.aspect) + extra;
      if (sc < best)
        best = sc, *s = t, found = 1;
      continue;
    }
    struct mx_screen f[48];
    int n = mx_screen_fills(c, mode, f, 48);
    for (int i = 0; i < n && i < 48; i++) {
      t = f[i];
      if (aspect && off(t.aspect, aspect) > 10) {
        /* narrowed or shortened to the shape asked for: black borders */
        if (aspect < t.aspect)
          t.w = (u16)((u32)t.w * aspect / t.aspect);
        else
          t.h = (u16)((u32)t.h * t.aspect / aspect);
        finish(c, mode, &t);
      }
      u32 sc = ratio(t.w, w) + ratio(t.h, h) + extra;
      if (sc < best)
        best = sc, *s = t, found = 1;
    }
  }
  return found ? 0 : -1;
}

void mx_screen_video(const struct mx_screen_conf *c, const struct mx_screen *s,
                     struct mx_video_mode *m) {
  const struct mx_video_mode *b = areas[s->mode].m;
  u32 vck, lines;
  area_of(c, s->mode, &vck, &lines);
  *m = *b;
  m->vck_w = (u16)vck;
  m->lines = (u16)lines;
  m->dx = (u16)((s32)b->dx + ((s32)b->vck_w - (s32)vck) / 2);
  s32 dy = (s32)b->dy + ((s32)b->lines - (s32)lines) / 2;
  m->dy = (u16)(b->interlace ? dy & ~1 : dy);
  m->ffmd = b->interlace ? s->ffmd : 0;
}
