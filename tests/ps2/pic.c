/* Still pictures (<mx/pic.h>): JPEG and PNG files of several kinds
   (tools/pic-fixtures.sh) against ffmpeg's decoding of them. PNG must be the
   same pixels; JPEG's inverse transforms differ by a step or two. */

#include <mx/pic.h>

#include "t.h"


static const u8 j444[] = {
#embed "data/pic_444.jpg"
};
static const u8 j444_yuv[] = {
#embed "data/pic_444.yuv"
};
static const u8 j420[] = {
#embed "data/pic_420.jpg"
};
static const u8 j420_yuv[] = {
#embed "data/pic_420.yuv"
};
static const u8 j422[] = {
#embed "data/pic_422.jpg"
};
static const u8 j422_yuv[] = {
#embed "data/pic_422.yuv"
};
static const u8 jgrey[] = {
#embed "data/pic_grey.jpg"
};
static const u8 jgrey_yuv[] = {
#embed "data/pic_grey.yuv"
};
static const u8 jrst[] = {
#embed "data/pic_rst.jpg"
};
static const u8 jrst_yuv[] = {
#embed "data/pic_rst.yuv"
};
static const u8 prgb[] = {
#embed "data/pic_rgb.png"
};
static const u8 prgb_px[] = {
#embed "data/pic_rgb.rgba"
};
static const u8 prgba[] = {
#embed "data/pic_rgba.png"
};
static const u8 prgba_px[] = {
#embed "data/pic_rgba.rgba"
};
static const u8 ppal[] = {
#embed "data/pic_pal.png"
};
static const u8 ppal_px[] = {
#embed "data/pic_pal.rgba"
};
static const u8 pgrey[] = {
#embed "data/pic_grey.png"
};
static const u8 pgrey_px[] = {
#embed "data/pic_grey.rgba"
};

static int clamp(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* A JPEG against ffmpeg's planes: Y of w x h, then Cb and Cr with the chroma
   hd x vd times smaller (0: grey), converted as the decoder does. */
static void jpeg(const char *name, const u8 *file, u32 size, const u8 *yuv, u32 yuv_size, u32 w, u32 h,
                 u32 hd, u32 vd) {
  struct mx_pic p;
  u32 t0 = mx_count();
  int r = mx_pic_decode(file, size, &p);
  u32 t = mx_count() - t0;
  CHECK_EQ(r, MX_PIC_OK);
  if (r)
    return;
  CHECK_EQ(p.width, w);
  CHECK_EQ(p.height, h);
  CHECK_EQ(p.alpha, 0);
  u32 cw = hd ? (w + hd - 1) / hd : 0, chh = hd ? (h + vd - 1) / vd : 0;
  CHECK_EQ(yuv_size, w * h + 2 * cw * chh);
  int max = 0;
  u32 sum = 0;
  for (u32 y = 0; y < h; y++)
    for (u32 x = 0; x < w; x++) {
      int l = yuv[y * w + x], u = 0, v = 0;
      if (hd) {
        u = yuv[w * h + (y / vd) * cw + x / hd] - 128;
        v = yuv[w * h + cw * chh + (y / vd) * cw + x / hd] - 128;
      }
      int want[3] = {clamp(l + ((v * 91881 + 32768) >> 16)), clamp(l - ((u * 22554 + v * 46802 + 32768) >> 16)),
                     clamp(l + ((u * 116130 + 32768) >> 16))};
      u32 got = p.pixels[y * w + x];
      CHECK_EQ(got >> 24, 0x80);
      for (int c = 0; c < 3; c++) {
        int d = (int)(got >> (c * 8) & 255) - want[c];
        d = d < 0 ? -d : d;
        max = d > max ? d : max;
        sum += (u32)d;
      }
    }
  u32 mean = sum * 100 / (w * h * 3);
  mx_printf("pic: %s: %u x %u, largest difference to ffmpeg's %d, mean %u.%02u; %u us\n", name, w, h, max,
            mean / 100, mean % 100, (u32)((u64)t * 1000000 / 294912000));
  CHECK(max <= 6);
  CHECK(mean < 100);
  mx_pic_free(&p);
}

static void png(const char *name, const u8 *file, u32 size, const u8 *px, u32 w, u32 h, int alpha) {
  struct mx_pic p;
  u32 t0 = mx_count();
  int r = mx_pic_decode(file, size, &p);
  u32 t = mx_count() - t0;
  CHECK_EQ(r, MX_PIC_OK);
  if (r)
    return;
  CHECK_EQ(p.width, w);
  CHECK_EQ(p.height, h);
  CHECK_EQ(p.alpha, alpha);
  u32 bad = 0;
  for (u32 i = 0; i < w * h; i++) {
    u32 want = ((u32)px[i * 4 + 3] + 1) >> 1 << 24 | (u32)px[i * 4 + 2] << 16 | (u32)px[i * 4 + 1] << 8 | px[i * 4];
    bad += p.pixels[i] != want;
  }
  mx_printf("pic: %s: %u x %u, %u pixels differ; %u us\n", name, w, h, bad,
            (u32)((u64)t * 1000000 / 294912000));
  CHECK_EQ(bad, 0);
  mx_pic_free(&p);
}

int main(void) {
  jpeg("JPEG 4:4:4", j444, sizeof j444, j444_yuv, sizeof j444_yuv, 48, 32, 1, 1);
  jpeg("JPEG 4:2:0", j420, sizeof j420, j420_yuv, sizeof j420_yuv, 50, 38, 2, 2);
  jpeg("JPEG 4:2:2", j422, sizeof j422, j422_yuv, sizeof j422_yuv, 48, 32, 2, 1);
  jpeg("JPEG grey", jgrey, sizeof jgrey, jgrey_yuv, sizeof jgrey_yuv, 48, 32, 0, 0);
  jpeg("JPEG 4:2:0 with restarts", jrst, sizeof jrst, jrst_yuv, sizeof jrst_yuv, 48, 32, 2, 2);
  png("PNG RGB", prgb, sizeof prgb, prgb_px, 48, 32, 0);
  png("PNG RGBA", prgba, sizeof prgba, prgba_px, 48, 32, 1);
  png("PNG palette", ppal, sizeof ppal, ppal_px, 48, 32, 0);
  png("PNG grey", pgrey, sizeof pgrey, pgrey_px, 48, 32, 0);

  /* what is no picture, and pictures cut off */
  struct mx_pic p;
  u16 w, h;
  CHECK_EQ(mx_pic_decode(prgb_px, sizeof prgb_px, &p), MX_PIC_FORMAT);
  CHECK_EQ(mx_pic_decode(j420, sizeof j420 / 2, &p), MX_PIC_DATA);
  CHECK_EQ(mx_pic_decode(prgb, sizeof prgb / 2, &p), MX_PIC_DATA);
  CHECK_EQ(mx_pic_size(j420, sizeof j420, &w, &h), MX_PIC_OK);
  CHECK_EQ(w, 50);
  CHECK_EQ(h, 38);
  CHECK_EQ(mx_pic_size(ppal, sizeof ppal, &w, &h), MX_PIC_OK);
  CHECK_EQ(w, 48);
  return t_done("pic");
}
