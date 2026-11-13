/* MPEG video (<mx/mpeg.h>) on streams made by ffmpeg (tools/mpeg-fixtures.sh),
   against ffmpeg's own decoding of them: its Y, Cb, Cr through the manual's
   colour conversion (8.6.1). The IDCTs differ by a step here and there, so
   the pictures are compared within a margin. */

#include <mx/mpeg.h>
#include <mx/mpegps.h>

#include "t.h"

static const u8 a_m2v[] = {
#embed "data/mpeg_a.m2v"
};
static const u8 a_yuv[] = {
#embed "data/mpeg_a.yuv"
};
static const u8 b_m2v[] = {
#embed "data/mpeg_b.m2v"
};
static const u8 b_yuv[] = {
#embed "data/mpeg_b.yuv"
};
static const u8 c_m1v[] = {
#embed "data/mpeg_c.m1v"
};
static const u8 c_yuv[] = {
#embed "data/mpeg_c.yuv"
};

static const u8 a_mpg[] = {
#embed "data/mpeg_a.mpg"
};
static const u8 a_pcm[] = {
#embed "data/mpeg_a.pcm"
};

#define MBS 20 /* the largest: 5 x 4 */
static u8 buf[8192] MX_ALIGNED(64) MX_CORE_BSS;
static u32 out[MBS * 256] MX_ALIGNED(64) MX_CORE_BSS;
static u64 tags[MX_MPEG_CHAIN_QW(MBS) * 2] MX_ALIGNED(64) MX_CORE_BSS;

/* "The lower 6 bits are rounded down": of the product's magnitude, so
   towards 0 (the PS2's answer; docs/ipu.md) */
static int down(int v) { return v < 0 ? -(-v & ~0x3F) : v & ~0x3F; }
static u32 clip8(int v) { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); }
static u32 csc_ref(int y, int cb, int cr) {
  int cr1 = cr - 128, cb1 = cb - 128;
  int cr4 = down(0xCC * cr1), cr5 = down(0x68 * cr1);
  int cb4 = down(0x32 * cb1), cb5 = down(0x102 * cb1);
  int y3 = (0x95 * (y - 16)) & ~0x3F;
  return clip8((y3 + cb5 + 64) >> 7) << 16 | clip8((y3 - cb4 - cr5 + 64) >> 7) << 8 |
         clip8((y3 + cr4 + 64) >> 7);
}

/* The decoded macroblocks against a frame of the reference: the largest and
   the mean difference of a colour. */
static void compare(const struct mx_mpeg *m, const u8 *yuv, int rgb16, int *max, int *mean) {
  u32 w = m->width, h = m->height, sum = 0;
  const u8 *cb = yuv + w * h, *cr = cb + (w / 2) * (h / 2);
  *max = 0;
  for (u32 y = 0; y < h; y++)
    for (u32 x = 0; x < w; x++) {
      u32 mb = (y >> 4) * m->mbw + (x >> 4), px = (y & 15) * 16 + (x & 15);
      u32 want = csc_ref(yuv[y * w + x], cb[(y / 2) * (w / 2) + x / 2], cr[(y / 2) * (w / 2) + x / 2]);
      u32 got;
      if (rgb16) {
        u32 v = ((const u16 *)out)[mb * 256 + px];
        got = (v & 31) << 3 | (v >> 5 & 31) << 11 | (v >> 10 & 31) << 19;
        want &= 0xF8F8F8;
      } else {
        got = out[mb * 256 + px];
      }
      for (int c = 0; c < 24; c += 8) {
        int d = (int)(got >> c & 255) - (int)(want >> c & 255);
        if (d < 0)
          d = -d;
        if (d > *max)
          *max = d;
        sum += (u32)d;
      }
    }
  *mean = (int)(sum * 100 / (w * h * 3));
}

static void stream(const char *name, const u8 *es, u32 es_size, const u8 *yuv, u32 yuv_size, u32 w,
                   u32 h, int mpeg2, u32 flags, int max_ok, int mean_ok) {
  /* the stream, an end code and what must follow it; at an odd offset, as
     in a file's buffer */
  u32 start = 5, size = start + es_size;
  memset(buf, 0, sizeof buf);
  memcpy(buf + start, es, es_size);
  buf[size + 2] = 1;
  buf[size + 3] = 0xB7;
  size += 4 + MX_MPEG_AHEAD;
  mx_dcache_writeback(buf, sizeof buf);

  struct mx_mpeg m;
  mx_mpeg_init(&m, flags);
  u32 pos = start, frames = 0, frame_bytes = w * h * 3 / 2;
  for (;;) {
    int r = mx_mpeg_headers(&m, buf, size, &pos);
    if (r != MX_MPEG_PICTURE) {
      CHECK_EQ(r, MX_MPEG_END);
      break;
    }
    CHECK_EQ(m.width, w);
    CHECK_EQ(m.height, h);
    CHECK_EQ(m.mbw, (w + 15) / 16);
    CHECK_EQ(m.mbh, m.progressive ? (h + 15) / 16 : (h + 31) / 32 * 2);
    CHECK_EQ(m.mpeg2, mpeg2);
    CHECK_EQ(m.type, MX_MPEG_I);
    CHECK_EQ(m.rate_num, 25);
    CHECK_EQ(m.rate_den, 1);
    memset(out, 0, sizeof out);
    mx_dcache_writeback(out, sizeof out);
    r = mx_mpeg_picture(&m, buf, size, &pos, out);
    mx_dcache_invalidate(out, sizeof out);
    CHECK_EQ(r, MX_MPEG_PICTURE);
    CHECK_EQ(m.decoded, m.mbw * m.mbh);
    CHECK_EQ(m.errors, 0);
    CHECK_EQ(buf[pos] << 16 | buf[pos + 1] << 8 | buf[pos + 2], 1); /* at a start code */
    if ((frames + 1) * frame_bytes <= yuv_size) {
      int max, mean;
      compare(&m, yuv + frames * frame_bytes, flags & MX_IPU_RGB16 ? 1 : 0, &max, &mean);
      mx_printf("mpeg: %s frame %u: difference to ffmpeg's: largest %d, mean %d.%02d\n", name, frames,
                max, mean / 100, mean % 100);
      CHECK(max <= max_ok);
      CHECK(mean < (flags & MX_IPU_RGB16 ? 400 : mean_ok));
    }
    frames++;
    if (frames > 8)
      break;
  }
  CHECK_EQ(frames, 2);
  CHECK_EQ(frames * frame_bytes, yuv_size);

  u32 qw = mx_mpeg_chain(tags, &m, out, 0, 2, flags & MX_IPU_RGB16 ? MX_GS_PSM_CT16 : MX_GS_PSM_CT32,
                         0, 0, MX_TAG_END);
  CHECK_EQ(qw, MX_MPEG_CHAIN_QW(m.mbw * m.mbh));
}

/* The buffer ends inside a picture: MX_MPEG_MORE, and the picture decodes
   when the rest is there. */
static void more(void) {
  u32 size = sizeof a_m2v;
  memset(buf, 0, sizeof buf);
  memcpy(buf, a_m2v, size);
  buf[size + 2] = 1;
  buf[size + 3] = 0xB7;
  size += 4 + MX_MPEG_AHEAD;
  mx_dcache_writeback(buf, sizeof buf);
  struct mx_mpeg m;
  mx_mpeg_init(&m, 0);
  u32 pos = 0;
  CHECK_EQ(mx_mpeg_headers(&m, buf, 30, &pos), MX_MPEG_MORE); /* inside the headers */
  CHECK_EQ(mx_mpeg_headers(&m, buf, size, &pos), MX_MPEG_PICTURE);
  u32 p = pos;
  CHECK_EQ(mx_mpeg_picture(&m, buf, pos + 100, &p, out), MX_MPEG_MORE);
  CHECK_EQ(p, pos);
  CHECK_EQ(mx_mpeg_picture(&m, buf, size, &pos, out), MX_MPEG_PICTURE);
  mx_dcache_invalidate(out, sizeof out);
  CHECK_EQ(m.decoded, m.mbw * m.mbh);
  int max, mean;
  compare(&m, a_yuv, 0, &max, &mean);
  mx_printf("mpeg: after a picture given up: difference largest %d, mean %d.%02d\n", max, mean / 100,
            mean % 100);
  /* PCSX2: the first block after an IDEC that was given up is off (docs/pcsx2.md) */
  CHECK(max <= (mx_ipu_qsc_is_scale() ? 32 : 8));
}

/* A slice damaged: the rest of the picture is still decoded. */
static void damaged(void) {
  u32 size = sizeof a_m2v;
  memset(buf, 0, sizeof buf);
  memcpy(buf, a_m2v, size);
  buf[size + 2] = 1;
  buf[size + 3] = 0xB7;
  size += 4 + MX_MPEG_AHEAD;
  struct mx_mpeg m;
  mx_mpeg_init(&m, 0);
  u32 pos = 0;
  CHECK_EQ(mx_mpeg_headers(&m, buf, size, &pos), MX_MPEG_PICTURE);
  /* the second slice: its data becomes ones (no code is 24 ones) */
  u32 p = pos + 4;
  while (!(buf[p] == 0 && buf[p + 1] == 0 && buf[p + 2] == 1))
    p++;
  memset(buf + p + 6, 0xFF, 6);
  mx_dcache_writeback(buf, sizeof buf);
  int r = mx_mpeg_picture(&m, buf, size, &pos, out);
  CHECK_EQ(r, MX_MPEG_PICTURE);
  CHECK_EQ(m.errors, 1);
  /* the PS2 loses the slice from the damage on; PCSX2 goes on through it and
     loses the slices after it too */
  CHECK(m.decoded < m.mbw * m.mbh);
  CHECK(m.decoded >= m.mbw * (m.mbh - 1) || mx_ipu_qsc_is_scale());
  mx_printf("mpeg: damaged slice: %u of %u macroblocks, %u error\n", m.decoded, m.mbw * m.mbh, m.errors);
  /* and the next picture is whole */
  CHECK_EQ(mx_mpeg_headers(&m, buf, size, &pos), MX_MPEG_PICTURE);
  CHECK_EQ(mx_mpeg_picture(&m, buf, size, &pos, out), MX_MPEG_PICTURE);
  CHECK_EQ(m.errors, 0);
  CHECK_EQ(m.decoded, m.mbw * m.mbh);
}

/* The program stream of stream a with sound: its packets taken apart give
   the video's bytes and the samples back. */
static void program_stream(void) {
  static u8 snd[sizeof a_pcm + 64];
  u32 pos = 0, v = 0, a = 0, packets = 0;
  struct mx_ps_packet p;
  /* cut off inside the first packet */
  CHECK_EQ(mx_ps_next(a_mpg, 40, &pos, &p), MX_PS_MORE);
  int r, first_a = 1;
  while ((r = mx_ps_next(a_mpg, sizeof a_mpg, &pos, &p)) != MX_PS_MORE && r != MX_PS_END) {
    packets++;
    if (r == MX_PS_VIDEO && v + p.bytes <= sizeof buf) {
      memcpy(buf + v, p.data, p.bytes);
      v += p.bytes;
    } else if (r == MX_PS_LPCM && a + p.bytes <= sizeof snd) {
      if (first_a) {
        CHECK_EQ(p.has_pts, 1);
        CHECK_EQ(p.rate, 48000);
        CHECK_EQ(p.channels, 2);
        CHECK_EQ(p.bits, 16);
        CHECK_EQ(p.stream, 0);
      }
      first_a = 0;
      memcpy(snd + a, p.data, p.bytes);
      a += p.bytes;
    }
  }
  CHECK_EQ(v, sizeof a_m2v);
  CHECK(!memcmp(buf, a_m2v, sizeof a_m2v));
  CHECK_EQ(a, sizeof a_pcm);
  CHECK(!memcmp(snd, a_pcm, sizeof a_pcm));
  mx_printf("mpeg: program stream: %u packets, %u bytes of video, %u of sound\n", packets, v, a);
}

int main(void) {
  CHECK_EQ(mx_ipu_init(), 0);
  /* PCSX2 takes the quantizer matrix in the order of the scan, which is
     wrong for the alternate scan with a matrix that is not flat (docs/pcsx2.md) */
  int pcsx2 = mx_ipu_qsc_is_scale();
  stream("a (MPEG-2)", a_m2v, sizeof a_m2v, a_yuv, sizeof a_yuv, 64, 48, 1, 0, 8, 100);
  stream("b (MPEG-2, field DCT, alternate scan, own matrix)", b_m2v, sizeof b_m2v, b_yuv,
         sizeof b_yuv, 72, 40, 1, 0, pcsx2 ? 64 : 8, pcsx2 ? 500 : 100);
  stream("c (MPEG-1)", c_m1v, sizeof c_m1v, c_yuv, sizeof c_yuv, 64, 48, 0, 0, 8, 100);
  stream("a, RGB16", a_m2v, sizeof a_m2v, a_yuv, sizeof a_yuv, 64, 48, 1, MX_IPU_RGB16, 16, 0);
  more();
  damaged();
  program_stream();
  mx_ipu_exit();
  return t_done("mpeg");
}
