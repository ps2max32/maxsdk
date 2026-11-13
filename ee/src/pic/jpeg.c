/* JPEG (ITU T.81), baseline and extended sequential with Huffman codes, on
   the EE (<mx/pic.h>). A minimum coded unit at a time: its blocks decoded
   and transformed back, then its pixels converted and written. */

#include <mx/pic.h>
#include <mx/string.h>

struct huff {
  u8 fast[512];    /* by the next 9 bits: the code's index in val, 255: longer than 9 */
  u8 len[256];     /* a code's length, by index */
  u8 val[256];
  s32 maxcode[18]; /* by length: the first code of 16 bits that is longer */
  s32 delta[17];   /* by length: index = code + delta */
};

struct jpg {
  const u8 *p, *end;
  u32 buf;   /* the next bits, from bit 31 down */
  int bits;  /* how many of them */
  u8 marker; /* a marker met in the scan's data (0: none) */
  u16 w, h;
  u8 ncomp, hmax, vmax;
  struct {
    u8 id, h, v, tq, td, ta;
    s32 pred;
  } c[3];
  u16 q[4][64];     /* in the order of the scan (zigzag) */
  struct huff h_[4]; /* DC 0, 1; AC 0, 1 */
  u32 restart;
};

static const u8 zigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
                              12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
                              35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
                              58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

/* ---- the scan's bits --------------------------------------------------------- */

static void grow(struct jpg *j) {
  while (j->bits <= 24) {
    u32 b = 0;
    if (!j->marker && j->p < j->end) {
      b = *j->p++;
      if (b == 0xFF) {
        u32 c = j->p < j->end ? *j->p++ : 0xD9;
        while (c == 0xFF && j->p < j->end)
          c = *j->p++;
        if (c) { /* a marker: the scan's data ends here, zeros follow */
          j->marker = (u8)c;
          b = 0;
        }
      }
    }
    j->buf |= b << (24 - j->bits);
    j->bits += 8;
  }
}

static u32 take(struct jpg *j, int n) {
  if (j->bits < n)
    grow(j);
  u32 v = j->buf >> (32 - n);
  j->buf <<= n;
  j->bits -= n;
  return v;
}

/* a Huffman code's value; -1 for no code */
static int symbol(struct jpg *j, const struct huff *h) {
  if (j->bits < 16)
    grow(j);
  u32 k = h->fast[j->buf >> 23];
  if (k < 255) {
    int n = h->len[k];
    j->buf <<= n;
    j->bits -= n;
    return h->val[k];
  }
  u32 top = j->buf >> 16;
  int n = 10;
  while (n <= 16 && (s32)top >= h->maxcode[n])
    n++;
  if (n > 16)
    return -1;
  k = (u32)((s32)(j->buf >> (32 - n)) + h->delta[n]);
  j->buf <<= n;
  j->bits -= n;
  return k < 256 ? h->val[k] : -1;
}

/* n more bits as the signed value they stand for (T.81, F.2.2.1) */
static int extend(struct jpg *j, int n) {
  int v = (int)take(j, n);
  return v < 1 << (n - 1) ? v - (1 << n) + 1 : v;
}

static int build(struct huff *h, const u8 *counts, const u8 *vals, u32 n) {
  u32 k = 0, code = 0;
  memset(h->fast, 255, sizeof h->fast);
  for (int len = 1; len <= 16; len++) {
    h->delta[len] = (s32)k - (s32)code;
    for (u32 i = 0; i < counts[len - 1]; i++, k++, code++) {
      if (k >= n || k >= 256)
        return -1;
      h->len[k] = (u8)len;
      h->val[k] = vals[k];
      if (len <= 9)
        for (u32 f = code << (9 - len); f < (code + 1) << (9 - len); f++)
          h->fast[f] = (u8)(k < 255 ? k : 255);
    }
    h->maxcode[len] = (s32)(code << (16 - len));
    code <<= 1;
  }
  h->maxcode[17] = 0x7FFFFFFF;
  return 0;
}

/* ---- a block ----------------------------------------------------------------- */

/* The inverse DCT (the integer one of Loeffler, Ligtenberg and Moschytz, 13
   bits of fraction): 64 coefficients in the block's own order to 64 samples
   of 0 .. 255 at out, stride apart. */
#define F(x) ((s32)((x) * 8192 + 0.5))
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                               \
  s32 z1 = (s2 + s6) * F(0.541196100), t2 = z1 - s6 * F(1.847759065), t3 = z1 + s2 * F(0.765366865); \
  s32 t0 = (s0 + s4) << 13, t1 = (s0 - s4) << 13;                                             \
  s32 e0 = t0 + t3, e3 = t0 - t3, e1 = t1 + t2, e2 = t1 - t2;                                 \
  s32 o0 = s7, o1 = s5, o2 = s3, o3 = s1;                                                     \
  s32 y1 = o0 + o3, y2 = o1 + o2, y3 = o0 + o2, y4 = o1 + o3, y5 = (y3 + y4) * F(1.175875602); \
  o0 *= F(0.298631336), o1 *= F(2.053119869), o2 *= F(3.072711026), o3 *= F(1.501321110);     \
  y1 *= -F(0.899976223), y2 *= -F(2.562915447), y3 = y3 * -F(1.961570560) + y5,               \
                                                y4 = y4 * -F(0.390180644) + y5;               \
  o0 += y1 + y3, o1 += y2 + y4, o2 += y2 + y3, o3 += y1 + y4;

static u8 clamp(s32 v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void idct(const s16 *d, u8 *out, u32 stride) {
  s32 w[64];
  for (int i = 0; i < 8; i++) { /* columns */
    IDCT_1D(d[i], d[8 + i], d[16 + i], d[24 + i], d[32 + i], d[40 + i], d[48 + i], d[56 + i])
    const s32 r = 1 << 10;
    w[i] = (e0 + o3 + r) >> 11, w[56 + i] = (e0 - o3 + r) >> 11;
    w[8 + i] = (e1 + o2 + r) >> 11, w[48 + i] = (e1 - o2 + r) >> 11;
    w[16 + i] = (e2 + o1 + r) >> 11, w[40 + i] = (e2 - o1 + r) >> 11;
    w[24 + i] = (e3 + o0 + r) >> 11, w[32 + i] = (e3 - o0 + r) >> 11;
  }
  for (int i = 0; i < 8; i++, out += stride) { /* rows; 128 back on */
    const s32 *v = w + i * 8;
    IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
    const s32 r = (1 << 17) + (128 << 18);
    out[0] = clamp((e0 + o3 + r) >> 18), out[7] = clamp((e0 - o3 + r) >> 18);
    out[1] = clamp((e1 + o2 + r) >> 18), out[6] = clamp((e1 - o2 + r) >> 18);
    out[2] = clamp((e2 + o1 + r) >> 18), out[5] = clamp((e2 - o1 + r) >> 18);
    out[3] = clamp((e3 + o0 + r) >> 18), out[4] = clamp((e3 - o0 + r) >> 18);
  }
}

static int block(struct jpg *j, int ci, u8 *out, u32 stride) {
  s16 d[64];
  const u16 *q = j->q[j->c[ci].tq];
  memset(d, 0, sizeof d);
  int t = symbol(j, &j->h_[j->c[ci].td]);
  if (t < 0 || t > 15)
    return -1;
  j->c[ci].pred += t ? extend(j, t) : 0;
  d[0] = (s16)(j->c[ci].pred * q[0]);
  const struct huff *ac = &j->h_[2 + j->c[ci].ta];
  for (int k = 1; k < 64;) {
    int rs = symbol(j, ac);
    if (rs < 0)
      return -1;
    int r = rs >> 4, s = rs & 15;
    if (!s) {
      if (r != 15)
        break; /* the block's end */
      k += 16;
      continue;
    }
    k += r;
    if (k > 63)
      return -1;
    d[zigzag[k]] = (s16)(extend(j, s) * q[k]);
    k++;
  }
  idct(d, out, stride);
  return 0;
}

/* ---- the file ---------------------------------------------------------------- */

static u32 be16(const u8 *p) { return (u32)p[0] << 8 | p[1]; }

/* The segments up to the scan's data. frame_only: stop at the frame header. */
static int headers(struct jpg *j, int frame_only) {
  if (j->end - j->p < 4 || j->p[0] != 0xFF || j->p[1] != 0xD8)
    return MX_PIC_FORMAT;
  j->p += 2;
  for (;;) {
    while (j->p < j->end && *j->p != 0xFF)
      j->p++;
    while (j->p < j->end && *j->p == 0xFF)
      j->p++;
    if (j->end - j->p < 3)
      return MX_PIC_DATA;
    u32 m = *j->p++, len = be16(j->p);
    const u8 *s = j->p + 2, *e = j->p + len;
    if (len < 2 || e > j->end)
      return MX_PIC_DATA;
    if (m == 0xC0 || m == 0xC1) { /* the frame: baseline, extended sequential */
      if (len < 8 || s[0] != 8)
        return MX_PIC_FORMAT;
      j->h = (u16)be16(s + 1), j->w = (u16)be16(s + 3);
      j->ncomp = s[5];
      if ((j->ncomp != 1 && j->ncomp != 3) || len < 8u + j->ncomp * 3u)
        return MX_PIC_FORMAT;
      for (u32 i = 0; i < j->ncomp; i++) {
        j->c[i].id = s[6 + i * 3];
        j->c[i].h = s[7 + i * 3] >> 4, j->c[i].v = s[7 + i * 3] & 15;
        j->c[i].tq = s[8 + i * 3] & 3;
      }
      /* grey, or three components of one or two samples a unit each way
         (the chroma at full, half or quarter size, however it is written) */
      if (j->ncomp == 1)
        j->c[0].h = j->c[0].v = 1;
      j->hmax = j->vmax = 1;
      for (u32 i = 0; i < j->ncomp; i++) {
        if (j->c[i].h < 1 || j->c[i].h > 2 || j->c[i].v < 1 || j->c[i].v > 2)
          return MX_PIC_FORMAT;
        j->hmax = j->c[i].h > j->hmax ? j->c[i].h : j->hmax;
        j->vmax = j->c[i].v > j->vmax ? j->c[i].v : j->vmax;
      }
      if (frame_only)
        return MX_PIC_OK;
    } else if (m >= 0xC2 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
      return MX_PIC_FORMAT; /* progressive, lossless, arithmetic */
    } else if (m == 0xDB) { /* quantization tables */
      while (s < e) {
        u32 pq = s[0] >> 4, tq = s[0] & 3;
        if (s + 1 + (pq ? 128 : 64) > e)
          return MX_PIC_DATA;
        for (int i = 0; i < 64; i++)
          j->q[tq][i] = (u16)(pq ? be16(s + 1 + i * 2) : s[1 + i]);
        s += 1 + (pq ? 128 : 64);
      }
    } else if (m == 0xC4) { /* Huffman tables */
      while (s + 17 <= e) {
        u32 tc = s[0] >> 4, th = s[0] & 15, n = 0;
        for (int i = 0; i < 16; i++)
          n += s[1 + i];
        if (tc > 1 || th > 1 || s + 17 + n > e || build(&j->h_[tc * 2 + th], s + 1, s + 17, n))
          return MX_PIC_DATA;
        s += 17 + n;
      }
    } else if (m == 0xDD) {
      j->restart = len >= 4 ? be16(s) : 0;
    } else if (m == 0xDA) { /* the scan: all components at once */
      if (!j->w || len < 6u + j->ncomp * 2u || s[0] != j->ncomp)
        return j->w ? MX_PIC_FORMAT : MX_PIC_DATA;
      for (u32 i = 0; i < j->ncomp; i++) {
        if (s[1 + i * 2] != j->c[i].id)
          return MX_PIC_FORMAT;
        j->c[i].td = s[2 + i * 2] >> 4 & 1, j->c[i].ta = s[2 + i * 2] & 1;
      }
      j->p = e;
      return MX_PIC_OK;
    } else if (m == 0xD9) {
      return MX_PIC_DATA;
    }
    j->p = e;
  }
}

int mx_jpeg_size(const void *data, u32 bytes, u16 *width, u16 *height) {
  static struct jpg j; /* large: not on the stack */
  memset(&j, 0, sizeof j);
  j.p = data, j.end = j.p + bytes;
  int r = headers(&j, 1);
  *width = j.w, *height = j.h;
  return r;
}

int mx_jpeg_decode(const void *data, u32 bytes, u32 *out, u8 *alpha) {
  static struct jpg j;
  memset(&j, 0, sizeof j);
  j.p = data, j.end = j.p + bytes;
  int r = headers(&j, 0);
  if (r)
    return r;
  *alpha = 0;
  u32 mw = j.hmax * 8u, mh = j.vmax * 8u; /* a coded unit in pixels */
  u32 cols = (j.w + mw - 1) / mw, rows = (j.h + mh - 1) / mh, count = 0;
  u8 s[3][16 * 16]; /* a unit's samples by component, 16 a line */
  memset(s, 128, sizeof s);
  for (u32 my = 0; my < rows; my++)
    for (u32 mx = 0; mx < cols; mx++) {
      if (j.restart && count && count % j.restart == 0) {
        /* a restart marker: whole bytes, the predictions from nothing */
        j.buf = 0, j.bits = 0;
        if (!j.marker) {
          while (j.p + 1 < j.end && !(j.p[0] == 0xFF && j.p[1] >= 0xD0 && j.p[1] <= 0xD7))
            j.p++;
          j.p += 2;
        }
        j.marker = 0;
        j.c[0].pred = j.c[1].pred = j.c[2].pred = 0;
      }
      count++;
      for (u32 c = 0; c < j.ncomp; c++)
        for (u32 v = 0; v < j.c[c].v; v++)
          for (u32 h = 0; h < j.c[c].h; h++)
            if (block(&j, (int)c, s[c] + v * 8 * 16 + h * 8, 16))
              return MX_PIC_DATA;
      /* its pixels: of each component the sample that covers the pixel (no
         interpolation) */
      u32 x0 = mx * mw, y0 = my * mh;
      u32 hs[3], vs[3];
      for (u32 c = 0; c < 3; c++)
        hs[c] = j.hmax > j.c[c].h, vs[c] = j.vmax > j.c[c].v;
      for (u32 py = 0; py < mh && y0 + py < j.h; py++) {
        u32 *o = out + (y0 + py) * j.w + x0;
        for (u32 px = 0; px < mw && x0 + px < j.w; px++) {
          s32 l = s[0][(py >> vs[0]) * 16 + (px >> hs[0])], u = 0, w = 0;
          if (j.ncomp == 3) {
            u = s[1][(py >> vs[1]) * 16 + (px >> hs[1])] - 128;
            w = s[2][(py >> vs[2]) * 16 + (px >> hs[2])] - 128;
          }
          u32 red = clamp(l + ((w * 91881 + 32768) >> 16));
          u32 green = clamp(l - ((u * 22554 + w * 46802 + 32768) >> 16));
          u32 blue = clamp(l + ((u * 116130 + 32768) >> 16));
          o[px] = 0x80000000u | blue << 16 | green << 8 | red;
        }
      }
    }
  /* the end of image must follow: a file cut off has decoded zeros */
  if (j.marker != 0xD9) {
    while (j.p + 1 < j.end && !(j.p[0] == 0xFF && j.p[1] == 0xD9))
      j.p++;
    if (j.p + 1 >= j.end)
      return MX_PIC_DATA;
  }
  return MX_PIC_OK;
}
