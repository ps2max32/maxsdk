/* A movie in a frame loop (<mx/movie.h>): the way a game or a menu shows
   one. Each frame asks the movie whether a new picture is due and sends the
   picture into the frame's buffer.

     movie.elf host:clip.mpg [times]      (or a video stream alone, .m2v)

   Make one with: ffmpeg -i in -vf scale=640:448 -c:v mpeg2video -g 1 -bf 0 -q:v 4 \
                    -c:a pcm_s16be -ar 48000 -ac 2 -f vob -muxrate 20000000 clip.mpg
   (up to 640x512 on a PAL console; 640x384 there for a 16:9 picture).
   Without -g 1 -bf 0 the stream has P and B pictures: smaller, and several
   times the work to decode (<mx/movie.h>). */

#include <mx/movie.h>
#include <mx/mx.h>
#include <mx/video.h>

#define FB_W 640u
#define MBS_MAX 1280u /* 640 x 512 */

static u64 tags[2][(MX_MPEG_CHAIN_QW(MBS_MAX) + 3) * 2] MX_ALIGNED(64) MX_CORE_BSS;
static u64 clear[2 * 20] MX_ALIGNED(64) MX_CORE_BSS;

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "host:movie.mpg";
  int pal = mx_video_default() == &mx_video_pal;
  u32 fb_h = pal ? 512 : 448;
  struct mx_movie *mv = mx_ipu_init() < 0 ? 0 : mx_movie_open(path, 0);
  const struct mx_movie_info *in = mv ? mx_movie_info(mv) : 0;
  if (!mv || in->mbw * 16u > FB_W || in->mbh * 16u > fb_h) {
    mx_printf("movie: %s: cannot open it, no picture in it, or larger than %u x %u\n", path, FB_W, fb_h);
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("movie: %s: %u x %u, %u/%u pictures a second, %s\n", path, in->width, in->height,
            in->rate_num, in->rate_den, in->sound ? "with sound" : "no sound");

  mx_gs_init_mode(FB_W, fb_h, pal ? MX_GS_PAL : 0);
  for (int b = 0; b < 2; b++) {
    /* a frame's chain for buffer b: the picture in its middle, then FINISH */
    u32 n = mx_movie_chain(mv, tags[b], MX_GS_PAGE(mx_gs.rt[b].fbp), mx_gs.rt[b].fbw, MX_GS_PSM_CT32,
                           (FB_W - in->mbw * 16u) / 2, (fb_h - in->mbh * 16u) / 2, MX_TAG_CNT);
    u64 *c = tags[b] + 2 * n;
    *c++ = mx_dmatag(MX_TAG_END, 2, MX_PADDR_NONE, 0);
    *c++ = 0;
    c = mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_FINISH, 0);
    mx_dcache_writeback(tags[b], (u32)((u8 *)c - (u8 *)tags[b]));
  }
  /* both buffers black first */
  for (int i = 0; i < 2; i++) {
    int b = mx_gs_frame_begin();
    u64 *c = clear;
    *c++ = mx_dmatag(MX_TAG_END, 16, MX_PADDR_NONE, 0);
    *c++ = 0;
    c = mx_gs_e_draw_env(c, b, 0);
    c = mx_gs_e_clear(c, 0, 0, 0, 0);
    c = mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_FINISH, 0);
    mx_dcache_writeback(clear, sizeof clear);
    mx_gs_frame_submit(clear);
  }

  /* the frame loop: a frame a vblank, each with the picture that is current
     (both buffers get it in turn) */
  int times = argc > 2 ? argv[2][0] - '0' : 1;
  u32 frames = 0;
  for (;;) {
    int b = mx_gs_frame_begin();
    int r = mx_movie_update(mv);
    mx_gs_frame_submit(tags[b]);
    frames++;
    if (r < 0 && (--times <= 0 || mx_movie_rewind(mv) < 0)) /* over: again, or done */
      break;
  }
  mx_printf("movie: %u pictures in %u frames, %u slices in error; decoding a picture %u us%s\n",
            in->pictures, frames, in->errors, in->decode_us, in->predicted ? " (P and B pictures)" : "");
  if (in->sound)
    mx_printf("movie: sound: %d ms behind the picture at the end, %u jumps, %u times dry\n",
              in->sound_behind_ms, in->sound_jumps, in->sound_dry);
  u32 pictures = in->pictures;
  mx_movie_close(mv);
  mx_ipu_exit();
  MX_TEST_END(pictures ? 0 : 1);
  return 0;
}
