/* Film in HD (<mx/movie.h>): I pictures decoded on the IPU to RGB16 with
   dither and sent into one buffer in CT16, the one shown, with the file's
   sound. For 1920 x 1080 that buffer is all of GS memory (1920 x 1088: 510
   of the 512 pages).

     moviehd.elf host:clip.mpg [mode]      (or a video stream alone, .m2v)

   mode: 1080i (the default), 1080i50, 1080p24, 1080p25, 1080p30, 720p,
   720p50. The picture must fit the mode's buffer: 1920 x 1080, or 1280 x
   720. There is no 720p at 24 Hz (docs/video-plan.md): film in 720p is
   shown at 60 Hz, a picture 2 and 3 vblanks in turn.

   Make one with: ffmpeg -i in -vf scale=1920:1080 -c:v mpeg2video -g 1 -bf 0 -q:v 6 \
                    -c:a pcm_s16be -ar 48000 -ac 2 -f vob -muxrate 40000000 clip.mpg

   It prints what the steps take and how far the film fell behind. */

#include <mx/display.h>
#include <mx/movie.h>
#include <mx/mx.h>
#include <mx/video.h>

#define MBS (1920u / 16 * (1088u / 16)) /* the largest picture */

static u64 tags[MX_MPEG_CHAIN_QW(MBS) * 2] MX_ALIGNED(64) MX_CORE_BSS;

struct timing {
  u32 n, sum, worst; /* sum in 16 cycles */
};
static struct timing t_dec, t_send;
static void took(struct timing *t, u32 cycles) {
  t->n++;
  t->sum += cycles / 16;
  if (cycles > t->worst)
    t->worst = cycles;
}
static u32 us(u64 cycles) { return (u32)(cycles * 1000000 / 294912000); }
static void report(const char *what, const struct timing *t) {
  if (t->n)
    mx_printf("moviehd: %s: %u us (the longest %u us), %u times\n", what,
              us((u64)t->sum * 16 / t->n), us(t->worst), t->n);
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "host:moviehd.mpg";
  if (mx_ipu_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  struct mx_movie *mv = mx_movie_open(path, MX_MOVIE_RGB16);
  const struct mx_movie_info *in = mv ? mx_movie_info(mv) : 0;
  if (!mv || (u32)in->mbw * in->mbh > MBS) {
    mx_printf("moviehd: %s: cannot open it, no picture in it, or more than %u macroblocks\n", path, MBS);
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("moviehd: %s: %u x %u, %u/%u pictures a second, %s\n", path, in->width, in->height,
            in->rate_num, in->rate_den, in->sound ? "with sound" : "no sound");

  /* the mode by name; interlaced in FIELD mode: a field shows every other
     line of the buffer */
  static const struct {
    const char *name;
    const struct mx_video_mode *m;
  } modes[] = {{"1080i", &mx_video_1080i},     {"1080i50", &mx_video_1080i50},
               {"1080p24", &mx_video_1080p24}, {"1080p25", &mx_video_1080p25},
               {"1080p30", &mx_video_1080p30}, {"720p", &mx_video_720p},
               {"720p50", &mx_video_720p50}};
  u32 mi = 0;
  for (u32 i = 0; argc > 2 && i < sizeof modes / sizeof modes[0]; i++)
    if (!strcmp(argv[2], modes[i].name))
      mi = i;
  struct mx_video_mode mode = *modes[mi].m;
  mode.ffmd = 0;
  if (mx_gs_init_video(&mode) < 0) {
    mx_printf("moviehd: this console does not give %s (no vblank)\n", modes[mi].name);
    MX_TEST_END(1);
    return 1;
  }
  u32 vb = mx_gs.vsync;
  mx_sleep_us(1000000);
  mx_printf("moviehd: %s: %u vblanks a second (%u expected)\n", modes[mi].name, mx_gs.vsync - vb,
            mode.hz);
  /* the buffer: the mode's active area, a pixel a clock, whole macroblocks high */
  u32 W = mode.vck_w, H = (mode.lines + 15u) & ~15u;
  if (in->mbw * 16u > W || in->mbh * 16u > H) {
    mx_printf("moviehd: the picture is larger than %s's %u x %u\n", modes[mi].name, W, H);
    MX_TEST_END(1);
    return 1;
  }
  struct mx_rt rt;
  u32 page = mx_vram_alloc(mx_rt_pages(W, H, MX_GS_PSM_CT16));
  mx_rt_init(&rt, page, W, mode.lines, MX_GS_PSM_CT16, 0, MX_RT_NOZ);
  u32 n = mx_movie_chain(mv, tags, MX_GS_PAGE(page), rt.fbw, MX_GS_PSM_CT16,
                         (W - in->mbw * 16u) / 2 & ~15u, (H - in->mbh * 16u) / 2 & ~15u, MX_TAG_END);
  mx_printf("moviehd: the buffer: %u x %u, pages %u .. %u of 512; the chain %u quadwords\n", W, H, page,
            page + mx_rt_pages(W, H, MX_GS_PSM_CT16) - 1, n);
  struct mx_display d;
  mx_display_init(&d, &rt);

  /* A picture: decoded while the one before is on the screen; sent from the
     vblank it is due, top to bottom in less time than the screen takes to
     read the buffer, so the read-out never passes what is being written: no
     tear. A picture that is not decoded by its vblank goes out at the next
     one, and the film is that much behind. */
  u32 pictures = 0, late = 0, v0 = 0;
  s32 behind_min = 1 << 30, behind_max = -(1 << 30);
  for (;;) {
    u32 t0 = mx_count();
    if (!mx_movie_decode(mv))
      break;
    took(&t_dec, mx_count() - t0);

    /* the vblank this picture is due: the film's rate on the screen's */
    if (!pictures)
      v0 = mx_gs.vsync + 1;
    u32 due = v0 + late + (u32)((u64)pictures * mode.hz * in->rate_den / in->rate_num);
    if ((s32)(mx_gs.vsync - due) >= 0) {
      late += mx_gs.vsync - due + 1;
      due = mx_gs.vsync + 1;
    }
    for (u32 i = 0; (s32)(mx_gs.vsync - due) < 0; i++) {
      mx_sleep_us(200);
      if (i % 16 == 15)
        mx_movie_poll(mv);
    }
    t0 = mx_count();
    mx_dma_chain(MX_CH_GIF, tags, 0);
    mx_movie_shown(mv);
    mx_movie_poll(mv); /* the file's next pieces, while the picture is on its way */
    while (mx_dma_busy(MX_CH_GIF))
      mx_sleep_us(250);
    took(&t_send, mx_count() - t0);
    if (!pictures)
      mx_display_apply(&d); /* the first picture is in the buffer */
    pictures++;
    if (pictures > 48) {
      if (in->sound_behind_ms < behind_min)
        behind_min = in->sound_behind_ms;
      if (in->sound_behind_ms > behind_max)
        behind_max = in->sound_behind_ms;
    }
  }
  u32 fields = mx_gs.vsync - v0;
  /* the last picture stays while its sound plays out */
  for (int i = 0; i < 200 && in->sound && in->sound_left_ms; i++) {
    mx_movie_poll(mv);
    mx_sleep_us(5000);
  }
  mx_printf("moviehd: %u pictures in %u vblanks (%u expected), %u vblanks behind, %u slices in error\n",
            pictures, fields, pictures ? (u32)((u64)pictures * mode.hz * in->rate_den / in->rate_num) : 0,
            late, in->errors);
  mx_printf("moviehd: decoding a picture: %u us%s\n", in->decode_us, in->predicted ? " (P and B pictures)" : "");
  report("getting a picture (decoding, and the file when it is behind)", &t_dec);
  report("sending it to the GS, the file's packets apart beside it", &t_send);
  if (in->sound)
    mx_printf("moviehd: sound: behind the picture by %d .. %d ms after the first two seconds; "
              "%u jumps, %u times dry\n", behind_min, behind_max, in->sound_jumps, in->sound_dry);
  mx_movie_close(mv);
  mx_ipu_exit();
  MX_TEST_END(pictures ? 0 : 1);
  return 0;
}
