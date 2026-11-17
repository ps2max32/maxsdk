/* The video settings (kernel version 31, <mx/screen.h>).
     k59_video.elf          (status 0) the screens of made-up settings: the
       fills of 1080i, what mx_screen_pick chooses in stretch and native
     as the kernel's shell: k59_video.elf trial   (status 0) also: video.mode
       1080i not confirmed goes back to the region's mode after 15 s; 720p
       confirmed stays; mx_gs_init(640, 448) then sets 720p, the picture at
       2 x 1; the settings put back at the end
     as the kernel's shell: k59_video.elf keep <mode>|-   (status 0) video.mode
       and video.confirm set to it (removed: -), written to the card: the
       kernel's next start is in that mode */
#include <mx/dma.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/sif.h>
#include <mx/vm.h>
#include <mx/screen.h>
#include <mx/setting.h>
#include <mx/string.h>
#include <mx/thread.h>
#include "t.h"

static void show(const char *what, const struct mx_screen *s) {
  mx_printf("k59: %s: %s %ux%u x%u/%u%s, shape %u, cover %u, flags %x\n", what,
            mx_screen_name(s->mode), s->w, s->h, s->magh, s->magv, s->ffmd ? " frame" : "",
            s->aspect, s->cover, s->flags);
}

/* made-up settings: rgb, 240p, the HD modes allowed (dropped by rgb) */
static int rgb_get(const char *key, char *value, u32 size) {
  const char *v = !strcmp(key, "video.out")       ? "rgb"
                  : !strcmp(key, "video.mode")    ? "240p"
                  : !strcmp(key, "video.allowed") ? "288p 720p 1080p24,1080i50"
                                                  : 0;
  if (!v)
    return 0;
  mx_snprintf(value, size, "%s", v);
  return (int)strlen(v);
}

static void made_up(void) {
  struct mx_screen_conf c;
  memset(&c, 0, sizeof c);
  c.mode = MX_SCREEN_1080I;
  c.allowed = 1u << MX_SCREEN_480I | 1u << MX_SCREEN_1080I;
  struct mx_screen f[48], s;
  int n = mx_screen_fills(&c, MX_SCREEN_1080I, f, 48);
  mx_printf("k59: 1080i: %d fills\n", n);
  CHECK(n > 10);
  show("largest", &f[0]);
  CHECK(f[0].w == 1920 && f[0].h == 1080 && f[0].magh == 1);
  int has960 = 0, has640x360 = 0;
  for (int i = 0; i < n && i < 48; i++) {
    CHECK(f[i].flags & MX_SCREEN_F_FILL);
    CHECK(f[i].flags & MX_SCREEN_F_SHELL);
    has960 |= f[i].w == 960 && f[i].h == 540 && f[i].ffmd;
    has640x360 |= f[i].w == 640 && f[i].h == 360 && f[i].magh == 3 && f[i].magv == 3;
  }
  CHECK(has960);
  CHECK(has640x360);

  /* stretch: 640 x 448 as it is, the shape of 480i's (1299) nearest */
  CHECK_EQ(mx_screen_pick(&c, 640, 448, 1299, MX_SCREEN_EXACT, &s), 0);
  show("stretch 640x448", &s);
  CHECK(s.mode == MX_SCREEN_1080I && s.w == 640 && s.h == 448 && s.magh == 2);
  /* stretch: about 640 x 448 in 4:3, a filling size narrowed */
  CHECK_EQ(mx_screen_pick(&c, 640, 448, 1333, 0, &s), 0);
  show("stretch ~640x448 4:3", &s);
  CHECK(s.mode == MX_SCREEN_1080I && s.aspect > 1300 && s.aspect < 1370);
  /* 16:9 about 640 x 360: 640 x 360 at 3 x 3 */
  CHECK_EQ(mx_screen_pick(&c, 640, 360, 1778, 0, &s), 0);
  show("stretch ~640x360 16:9", &s);
  CHECK(s.w == 640 && s.h == 360 && (s.flags & MX_SCREEN_F_FILL));
  /* native: 480i shows 640 x 448 as it was made */
  c.native = 1;
  CHECK_EQ(mx_screen_pick(&c, 640, 448, 1299, MX_SCREEN_EXACT, &s), 0);
  show("native 640x448", &s);
  CHECK(s.mode == MX_SCREEN_480I && s.magh == 4 && !s.ffmd);
  /* a 512 x 448 buffer fills 480i at 5 */
  CHECK_EQ(mx_screen_fit(&c, MX_SCREEN_480I, 512, 448, 0, &s), 0);
  CHECK(s.magh == 5 && (s.flags & MX_SCREEN_F_FILL));
  /* overscan off: 704 x 480 fills */
  c.full = 1;
  CHECK_EQ(mx_screen_fit(&c, MX_SCREEN_480I, 704, 480, 0, &s), 0);
  show("480i 704x480", &s);
  CHECK(s.magh == 4 && (s.flags & MX_SCREEN_F_FILL) && s.aspect == 1333);
  c.full = 0;

  /* every name back to its mode */
  for (u32 m = 1; m < MX_SCREEN_MODES; m++)
    CHECK_EQ(mx_screen_mode_of(mx_screen_name(m)), m);
  /* 240p: 320 x 224 at 8 x 1 fills, 4:3 */
  CHECK_EQ(mx_screen_fit(&c, MX_SCREEN_240P, 320, 224, 0, &s), 0);
  show("240p 320x224", &s);
  CHECK(s.magh == 8 && s.magv == 1 && (s.flags & MX_SCREEN_F_FILL) && s.hz == 60);
  /* 1080p30: 1920 x 540 at 1 x 2, 16:9, 30 Hz */
  CHECK_EQ(mx_screen_fit(&c, MX_SCREEN_1080P30, 1920, 540, 0, &s), 0);
  show("1080p30 1920x540", &s);
  CHECK(s.magv == 2 && !s.ffmd && (s.flags & MX_SCREEN_F_FILL) && s.aspect == 1778 &&
        s.hz == 30);
  /* 1080i50 in stretch: its own fills */
  c.mode = MX_SCREEN_1080I50;
  c.allowed = 1u << MX_SCREEN_1080I50;
  CHECK_EQ(mx_screen_pick(&c, 640, 360, 1778, 0, &s), 0);
  CHECK(s.mode == MX_SCREEN_1080I50 && s.hz == 50);

  /* rgb: only the TV timings */
  struct mx_screen_conf r;
  mx_screen_conf_from(&r, rgb_get);
  CHECK_EQ(r.allowed & ~(1u << MX_SCREEN_480I | 1u << MX_SCREEN_576I | 1u << MX_SCREEN_240P |
                         1u << MX_SCREEN_288P),
           0);
  CHECK(r.allowed >> MX_SCREEN_240P & 1);
  CHECK_EQ(r.mode, MX_SCREEN_240P);
  memset(&c, 0, sizeof c);
}

static u32 mode_now(void) {
  struct mx_screen_conf c;
  mx_screen_conf(&c);
  return c.mode;
}

static void trial(void) {
  struct mx_screen_conf c;
  mx_screen_conf(&c);
  u32 region = c.pal ? MX_SCREEN_576I : MX_SCREEN_480I;
  mx_printf("k59: now %s, region %s\n", mx_screen_name(c.mode), mx_screen_name(region));
  CHECK_EQ(mx_setting_set("video.confirm", ""), 0);
  CHECK_EQ(mx_setting_set("video.mode", "1080i"), 0);
  mx_sleep_us(2000000);
  CHECK_EQ(mode_now(), MX_SCREEN_1080I);
  mx_printf("k59: 1080i set, not confirmed: waiting 16 s\n");
  mx_sleep_us(16000000);
  CHECK_EQ(mode_now(), region);
  mx_printf("k59: back to %s\n", mx_screen_name(mode_now()));

  CHECK_EQ(mx_setting_set("video.mode", "720p"), 0);
  CHECK_EQ(mx_setting_set("video.confirm", "720p component"), 0);
  mx_sleep_us(17000000);
  CHECK_EQ(mode_now(), MX_SCREEN_720P);
  mx_gs_init(640, 448);
  mx_printf("k59: mx_gs_init(640, 448): code %x, area %u x %u\n", mx_video.code, mx_video.vck_w,
            mx_video.lines);
  CHECK_EQ(mx_video.code, 0x52);

  /* put back */
  CHECK_EQ(mx_setting_set("video.mode", ""), 0);
  CHECK_EQ(mx_setting_set("video.confirm", ""), 0);
  mx_sleep_us(1500000);
  CHECK_EQ(mode_now(), region);
}

/* as the shell: video.mode and video.confirm set to mode (or removed: "-"),
   then the time for the kernel to write the settings */
static void keep(const char *mode) {
  char conf[32];
  mx_snprintf(conf, sizeof conf, "%s component", mode);
  int off = !strcmp(mode, "-");
  CHECK_EQ(mx_setting_set("video.mode", off ? "" : mode), 0);
  CHECK_EQ(mx_setting_set("video.confirm", off ? "" : conf), 0);
  mx_sleep_us(8000000);
  mx_printf("k59: video.mode %s kept\n", mode);
}

/* the kernel's console as it is in GS memory (page 0) to host:k59.raw:
   w x h pixels of psm (for a look at it on the PC) */
static void grab(u32 w, u32 h, u32 psm) {
  static u8 line[1920 * 4] MX_ALIGNED(64) MX_IRQ_DATA;
  u32 bpp = psm == MX_GS_PSM_CT32 ? 4 : 2;
  /* the GS's interrupts for the download (a GS reset keeps its memory) */
  mx_gs_init_video(&mx_video_ntsc);
  int fd = mx_fio_open("host:k59.raw", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return;
  for (u32 y = 0; y < h; y++) {
    if (mx_gs_download(line, 0, (w + 63) / 64, psm, 0, y, w, 1) < 0) {
      CHECK(0);
      break;
    }
    mx_fio_write(fd, line, w * bpp);
  }
  mx_fio_close(fd);
  mx_printf("k59: %u x %u, psm %x, saved\n", w, h, psm);
}

int main(int argc, char **argv) {
  if (argc > 3 && !strcmp(argv[1], "grab")) {
    u32 w = 0, h = 0;
    for (const char *p = argv[2]; *p; p++)
      w = w * 10 + (u32)(*p - '0');
    for (const char *p = argv[3]; *p; p++)
      h = h * 10 + (u32)(*p - '0');
    grab(w, h, argc > 4 ? (argv[4][0] == 's' ? MX_GS_PSM_CT16S : argv[4][0] == 'h' ? MX_GS_PSM_CT16 : MX_GS_PSM_CT32) : MX_GS_PSM_CT16S);
    return t_done("k59");
  }
  if (argc > 2 && !strcmp(argv[1], "keep")) {
    keep(argv[2]);
    return t_done("k59");
  }
  mx_printf("k59: kernel version %u\n", mx__kernel ? mx__kernel->version : 0);
  made_up();
  if (argc > 1 && !strcmp(argv[1], "trial"))
    trial();
  return t_done("k59");
}
