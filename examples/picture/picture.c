/* A JPEG or PNG picture from a file on the screen (<mx/pic.h>):

     picture.elf host:photo.jpg [seconds]

   Decoded on the EE and sent into both frame buffers, in the middle. Up to
   640 x 448 (640 x 512 on a PAL console). Prints what decoding took. */

#include <mx/mx.h>
#include <mx/pic.h>
#include <mx/video.h>

#define FB_W 640u

static u64 chain[2][64] MX_ALIGNED(64) MX_CORE_BSS;

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "host:picture.jpg";
  int seconds = argc > 2 ? argv[2][0] - '0' : 5;
  int pal = mx_video_default() == &mx_video_pal;
  u32 fb_h = pal ? 512 : 448;
  struct mx_pic p;
  u32 t0 = mx_count();
  int r = mx_pic_load(path, &p);
  u32 ms = (u32)((u64)(mx_count() - t0) * 1000 / 294912000);
  if (r || p.width > FB_W || p.height > fb_h) {
    mx_printf("picture: %s: %s\n", path,
              r == MX_PIC_FILE ? "cannot read it" : r == MX_PIC_FORMAT ? "not a JPEG or PNG this decodes"
              : r == MX_PIC_DATA ? "damaged" : r ? "no memory" : "larger than the screen");
    MX_TEST_END(1);
    return 1;
  }
  mx_printf("picture: %s: %u x %u%s, read and decoded in %u ms\n", path, p.width, p.height,
            p.alpha ? ", with alpha" : "", ms);

  mx_gs_init_mode(FB_W, fb_h, pal ? MX_GS_PAL : 0);
  u32 qwc = ((u32)p.width * p.height * 4 + 15) / 16;
  for (int i = 0; i < 2; i++) {
    /* a frame for each buffer: black, the picture by reference, FINISH */
    int b = mx_gs_frame_begin();
    u64 *c = chain[b];
    *c++ = mx_dmatag(MX_TAG_CNT, 9 + 5 + 5, MX_PADDR_NONE, 0);
    *c++ = 0;
    c = mx_gs_e_draw_env(c, b, 0);
    c = mx_gs_e_clear(c, 0, 0, 0, 0);
    c = mx_e_ad_tag(c, 4, 0);
    c = mx_e_ad(c, MX_GS_REG_BITBLTBUF,
                mx_gs_bitbltbuf(0, 0, 0, MX_GS_PAGE(mx_gs.rt[b].fbp), mx_gs.rt[b].fbw, MX_GS_PSM_CT32));
    c = mx_e_ad(c, MX_GS_REG_TRXPOS, mx_gs_trxpos(0, 0, (FB_W - p.width) / 2, (fb_h - p.height) / 2, 0));
    c = mx_e_ad(c, MX_GS_REG_TRXREG, mx_gs_trxreg(p.width, p.height));
    c = mx_e_ad(c, MX_GS_REG_TRXDIR, 0);
    for (u32 done = 0; done < qwc;) { /* an IMAGE tag takes 32767 quadwords */
      u32 n = qwc - done > 0x7FFF ? 0x7FFF : qwc - done;
      *c++ = mx_dmatag(MX_TAG_CNT, 1, MX_PADDR_NONE, 0);
      *c++ = 0;
      *c++ = mx_giftag(n, 0, -1, MX_GIF_FLG_IMAGE, 0);
      *c++ = 0;
      *c++ = mx_dmatag(MX_TAG_REF, n, mx_paddr_range((const u8 *)p.pixels + done * 16, n * 16), 0);
      *c++ = 0;
      done += n;
    }
    *c++ = mx_dmatag(MX_TAG_END, 2, MX_PADDR_NONE, 0);
    *c++ = 0;
    c = mx_e_ad(mx_e_ad_tag(c, 1, 1), MX_GS_REG_FINISH, 0);
    mx_dcache_writeback(chain[b], sizeof chain[b]);
    mx_gs_frame_submit(chain[b]);
  }
  mx_sleep_us((u32)seconds * 1000000);
  mx_pic_free(&p);
  MX_TEST_END(0);
  return 0;
}
