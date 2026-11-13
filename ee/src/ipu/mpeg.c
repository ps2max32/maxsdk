/* MPEG video on the IPU (<mx/mpeg.h>): the headers on the EE, I pictures a
   slice an IDEC command. */

#include <mx/cpu.h>
#include <mx/dma.h>
#include <mx/gif.h>
#include <mx/mpeg.h>
#include <mx/string.h>

/* ISO 13818-2, 6.3.11: the default intra matrix, in the order it would be
   sent (zigzag); the default non-intra one is 16 throughout. */
static const u8 default_intra[64] = {
    8,  16, 16, 19, 16, 19, 22, 22, 22, 22, 22, 22, 26, 24, 26, 27, 27, 27, 26, 26, 26, 26,
    27, 27, 27, 29, 29, 29, 34, 34, 34, 29, 29, 29, 27, 27, 29, 29, 32, 32, 34, 34, 37, 38,
    37, 35, 35, 34, 35, 38, 38, 40, 40, 40, 48, 48, 46, 46, 56, 56, 58, 69, 69, 83};

static const u16 rates[9][2] = {{0, 1},        {24000, 1001}, {24, 1}, {25, 1},      {30000, 1001},
                                {30, 1},       {50, 1},       {60000, 1001}, {60, 1}};

void mx_mpeg_init(struct mx_mpeg *m, u32 flags) {
  memset(m, 0, sizeof *m);
  m->flags = flags & (MX_IPU_RGB16 | MX_IPU_DITHER);
}

/* ---- the headers, on the EE ------------------------------------------------ */

/* The next 00 00 01 at or after from; size if there is none whose code
   byte is in the buffer too. */
static u32 next_code(const u8 *b, u32 from, u32 size) {
  for (u32 i = from; i + 3 < size; i++) {
    if (b[i + 2] > 1) {
      i += 2;
      continue;
    }
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)
      return i;
  }
  return size;
}

int mx_mpeg_next_type(const u8 *buf, u32 size, u32 pos) {
  for (;;) {
    pos = next_code(buf, pos, size);
    if (pos + 6 > size)
      return 0;
    if (buf[pos + 3] == 0x00)
      return buf[pos + 5] >> 3 & 7;
    pos += 4;
  }
}

struct bits {
  const u8 *p;
  u32 pos; /* in bits */
};
static u32 get(struct bits *b, u32 n) {
  u32 v = 0;
  for (; n; n--, b->pos++)
    v = v << 1 | (b->p[b->pos >> 3] >> (7 - (b->pos & 7)) & 1);
  return v;
}

static void set_matrix(struct mx_mpeg *m, int n, const u8 *from) {
  if (!m->iq_load[n] && !memcmp(m->iq[n], from, 64))
    return;
  memcpy(m->iq[n], from, 64);
  m->iq_load[n] = 1;
}
static void default_matrix(struct mx_mpeg *m, int n) {
  u8 d[64];
  if (n)
    memset(d, 16, 64);
  else
    memcpy(d, default_intra, 64);
  set_matrix(m, n, d);
}
/* load_intra_quantiser_matrix, load_non_intra_quantiser_matrix and what
   they bring (not byte aligned) */
static void get_matrices(struct mx_mpeg *m, struct bits *b, int defaults) {
  for (int n = 0; n < 2; n++) {
    if (get(b, 1)) {
      u8 q[64];
      for (int i = 0; i < 64; i++)
        q[i] = (u8)get(b, 8);
      set_matrix(m, n, q);
    } else if (defaults) {
      default_matrix(m, n);
    }
  }
}

int mx_mpeg_headers(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos) {
  u32 p = *pos;
  for (;;) {
    p = next_code(buf, p, size);
    *pos = p;
    /* the longest header: a sequence header with both matrices, 140 bytes */
    if (p + 4 > size)
      return MX_MPEG_MORE;
    u32 code = buf[p + 3];
    if (code >= 0x01 && code <= 0xAF) {
      if (!m->have_seq || !m->type)
        return MX_MPEG_BAD;
      return MX_MPEG_PICTURE;
    }
    if (code == 0xB7) {
      *pos = p + 4;
      return MX_MPEG_END;
    }
    u32 next = next_code(buf, p + 4, size);
    if (next == size) /* the header may go on past the buffer */
      return MX_MPEG_MORE;
    struct bits b = {buf + p + 4, 0};
    if (code == 0xB3) { /* sequence_header */
      m->width = (u16)get(&b, 12);
      m->height = (u16)get(&b, 12);
      m->aspect = (u8)get(&b, 4);
      u32 r = get(&b, 4);
      m->rate_num = rates[r > 8 ? 0 : r][0];
      m->rate_den = rates[r > 8 ? 0 : r][1];
      get(&b, 18 + 1 + 10 + 1);
      get_matrices(m, &b, 1);
      m->mbw = (u16)((m->width + 15) >> 4);
      m->mbh = (u16)((m->height + 15) >> 4);
      m->mpeg2 = 0; /* until its extension follows */
      m->progressive = 1;
      m->chroma = 1;
      m->have_seq = m->width && m->height;
    } else if (code == 0x00) { /* picture_header */
      m->temporal = (u16)get(&b, 10);
      m->type = (u8)get(&b, 3);
      get(&b, 16);
      /* MPEG-1's vectors (MPEG-2: 0 and 7 here, its own in the extension) */
      for (int i = 0; i < 2; i++) {
        m->full_pel[i] = 0;
        m->f_code[i][0] = m->f_code[i][1] = 1;
        if (m->type == MX_MPEG_P + i || m->type == MX_MPEG_B) {
          m->full_pel[i] = (u8)get(&b, 1);
          m->f_code[i][0] = m->f_code[i][1] = (u8)get(&b, 3);
        }
      }
      /* MPEG-1 has no more; MPEG-2's picture_coding_extension follows */
      m->structure = 3;
      m->dc_prec = 0;
      m->frame_dct = 1;
      m->conceal = m->q_type = m->vlc_fmt = m->alt_scan = 0;
      m->top_first = m->repeat_first = 0;
      m->progressive_frame = 1;
    } else if (code == 0xB5) { /* an extension */
      u32 id = get(&b, 4);
      if (id == 1) { /* sequence_extension */
        get(&b, 8);
        m->progressive = (u8)get(&b, 1);
        m->chroma = (u8)get(&b, 2);
        m->width |= (u16)(get(&b, 2) << 12);
        m->height |= (u16)(get(&b, 2) << 12);
        get(&b, 12 + 1 + 8 + 1);
        u32 n = get(&b, 2), d = get(&b, 5);
        m->rate_num *= n + 1;
        m->rate_den *= d + 1;
        m->mbw = (u16)((m->width + 15) >> 4);
        /* an interlaced sequence has whole macroblocks in each field */
        m->mbh = (u16)(m->progressive ? (m->height + 15) >> 4 : (m->height + 31) >> 5 << 1);
        m->mpeg2 = 1;
      } else if (id == 3) { /* quant_matrix_extension */
        get_matrices(m, &b, 0);
      } else if (id == 8) { /* picture_coding_extension */
        for (int i = 0; i < 4; i++)
          m->f_code[i >> 1][i & 1] = (u8)get(&b, 4);
        m->full_pel[0] = m->full_pel[1] = 0;
        m->dc_prec = (u8)get(&b, 2);
        m->structure = (u8)get(&b, 2);
        m->top_first = (u8)get(&b, 1);
        m->frame_dct = (u8)get(&b, 1);
        m->conceal = (u8)get(&b, 1);
        m->q_type = (u8)get(&b, 1);
        m->vlc_fmt = (u8)get(&b, 1);
        m->alt_scan = (u8)get(&b, 1);
        m->repeat_first = (u8)get(&b, 1);
        get(&b, 1);
        m->progressive_frame = (u8)get(&b, 1);
      }
    }
    /* group_of_pictures_header, user_data and the rest: nothing wanted */
    p = next;
  }
}

/* ---- a picture, on the IPU -------------------------------------------------- */

struct dec {
  const u8 *buf;
  u32 q0;        /* the first quadword given to the IPU */
  u32 fedq, endq; /* quadwords given so far (from the buffer's start), and there are */
};

#define FEED_MAX 0xFFFFu

static int more(struct dec *d) {
  if (d->fedq >= d->endq)
    return 0;
  u32 n = d->endq - d->fedq;
  if (n > FEED_MAX)
    n = FEED_MAX;
  mx_ipu_more(d->buf + d->fedq * 16, n);
  d->fedq += n;
  return 1;
}

static int busy(void) { return (mx_ipu_status() & (u32)MX_BIT(IPU_CTRL_BUSY)) != 0; }

/* FDEC; *starved when the buffer ends before its 32 bits */
static u32 fdec(struct dec *d, u32 fb, int *starved) {
  u32 v = mx_ipu_fdec(fb);
  if (!busy())
    return v;
  do {
    if (!more(d)) {
      *starved = 1;
      return 0;
    }
    mx_ipu_wait();
  } while (busy());
  return mx_ipu_top();
}

static int is_slice(u32 code) { return code >> 8 == 1 && (code & 0xFF) >= 0x01 && (code & 0xFF) <= 0xAF; }

/* Past a picture's slices on the EE: the next start code that is no slice. */
static int skip_slices(const u8 *buf, u32 size, u32 *pos) {
  u32 p = *pos;
  for (;;) {
    p = next_code(buf, p, size);
    if (p + 4 + MX_MPEG_AHEAD > size)
      return MX_MPEG_MORE;
    if (buf[p + 3] < 0x01 || buf[p + 3] > 0xAF)
      break;
    p += 4;
  }
  *pos = p;
  return MX_MPEG_SKIPPED;
}

/* ---- P and B pictures: a macroblock a BDEC, the prediction on the EE ---------- */

void mx_mpeg_refs(struct mx_mpeg *m, void *frames, void *block) {
  m->frames = frames;
  m->block = block ? block : (void *)0x70000000;
  m->ref[0] = 0, m->ref[1] = 1, m->spare = 2;
  m->nrefs = m->held = 0;
}

/* A picture is kept as its macroblocks, each 384 bytes as the IPU's RAW8: Y
   (16 lines of 16), Cb, Cr (8 lines of 8 each). CSC takes it as it lies, and
   a prediction reads fewer cache lines than from planes. */
static u8 *frame(const struct mx_mpeg *m, u32 f) { return m->frames + f * (u32)m->mbw * m->mbh * 384u; }

/* Eight pixels at a time in a 64-bit register: the EE's are that wide */
static inline u64 ld8(const u8 *p) {
  u64 v;
  __builtin_memcpy(&v, p, 8);
  return v;
}
static inline void st8(u8 *p, u64 v) { __builtin_memcpy(p, &v, 8); }
/* (a + b + 1) >> 1 of each byte */
static inline u64 avg2(u64 a, u64 b) { return (a | b) - (((a ^ b) & 0xFEFEFEFEFEFEFEFEull) >> 1); }
/* (a + b + c + d + 2) >> 2 of each byte: the upper six bits and the lower
   two apart, neither sum leaves its byte */
static inline u64 avg4(u64 a, u64 b, u64 c, u64 d) {
  const u64 hi = 0xFCFCFCFCFCFCFCFCull, lo = 0x0303030303030303ull;
  u64 h = ((a & hi) >> 2) + ((b & hi) >> 2) + ((c & hi) >> 2) + ((d & hi) >> 2);
  u64 l = (a & lo) + (b & lo) + (c & lo) + (d & lo) + 0x0202020202020202ull;
  return h + ((l >> 2) & lo);
}

/* Frame f to out as RGB. */
static void show(struct mx_mpeg *m, u32 f, void *out) {
  u32 mbq = m->flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32;
  u32 mbs = (u32)m->mbw * m->mbh, max = m->flags & MX_IPU_RGB16 ? 2047 : 1023;
  const u8 *src = frame(m, f);
  mx_dcache_writeback(src, mbs * 384u);
  for (u32 i = 0; i < mbs; i += max) {
    u32 n = mbs - i < max ? mbs - i : max;
    mx_ipu_csc(src + i * 384u, n, m->flags, (u8 *)out + i * mbq * 16);
  }
  mx_ipu_input_end();
}

int mx_mpeg_flush(struct mx_mpeg *m, void *out) {
  if (!m->frames || !m->held)
    return MX_MPEG_NONE;
  show(m, m->ref[1], out);
  m->held = 0;
  return MX_MPEG_PICTURE;
}

enum { MB_INTRA = 1, MB_PATTERN = 2, MB_BWD = 4, MB_FWD = 8, MB_QUANT = 16 };

struct pb {
  struct mx_mpeg *m;
  struct dec d;
  u32 w, used;   /* the stream's next 32 bits as FDEC gave them, and how many are taken */
  u8 valid, starved, bad;
  u8 *cur;             /* the picture being decoded */
  const u8 *from[2];   /* forward and backward reference */
  int pmv[2][2][2];    /* the vectors' predictors: [first, second][forward, backward][x, y] */
  u8 sel[2][2];        /* field prediction: the reference field of [field][direction] */
  u8 dirs, field;      /* the last macroblock's directions (MB_FWD | MB_BWD), field prediction */
  u32 qsc;
};

/* FDEC or VDEC, looked at: they end within cycles unless the stream's
   transfer has to go on. Returns what was decoded. */
static u32 quick(struct pb *p, u32 cmd) {
  MX_R32(MX_IPU_CMD) = cmd;
  for (int i = 0; i < 64; i++) {
    u64 v = MX_R64(MX_IPU_CMD);
    if (!(v >> 63))
      return (u32)v;
  }
  for (;;) {
    if (mx_ipu_wait() == MX_IPU_OK)
      return mx_ipu_result();
    if (!more(&p->d)) {
      p->starved = 1;
      return 0;
    }
  }
}
#define FDEC(fb) ((u32)MX_IPU_FDEC << 28 | (fb))
#define VDEC(tbl, fb) ((u32)MX_IPU_VDEC << 28 | (u32)(tbl) << 26 | (fb))

/* n bits of the stream (1 .. 16) */
static inline u32 bits(struct pb *p, u32 n) {
  if (!p->valid || p->used + n > 32) {
    p->w = quick(p, FDEC(p->valid ? p->used : p->used & 63));
    p->used = 0;
    p->valid = 1;
  }
  u32 v = (p->w << p->used) >> (32 - n);
  p->used += n;
  return v;
}
/* the next 32 bits, none taken */
static inline u32 peek(struct pb *p) {
  if (!p->valid || p->used) {
    p->w = quick(p, FDEC(p->used));
    p->used = 0;
    p->valid = 1;
  }
  return p->w;
}
/* a code of one of VDEC's tables: 0 for none */
static inline u32 vlc(struct pb *p, u32 tbl) {
  u32 v = quick(p, VDEC(tbl, p->used));
  p->used = 0;
  p->valid = 0;
  return v;
}

/* motion_vector's component: the code, its residual, the new vector from
   its predictor (ISO 13818-2, 7.6.3.1) */
static int vector(struct pb *p, int pred, u32 f_code) {
  u32 v = vlc(p, MX_IPU_VLC_MOTION);
  if (!v) {
    p->bad = 1;
    return pred;
  }
  int code = (s16)v, r_size = (int)f_code - 1, delta = code;
  if (code && r_size > 0) {
    int a = code < 0 ? -code : code;
    delta = ((a - 1) << r_size) + (int)bits(p, (u32)r_size) + 1;
    if (code < 0)
      delta = -delta;
  }
  int range = 16 << (r_size > 0 ? r_size : 0), vec = pred + delta;
  if (vec < -range)
    vec += 2 * range;
  else if (vec >= range)
    vec -= 2 * range;
  return vec;
}

/* motion_vectors(s) of a frame picture: one vector (frame prediction) or
   two with their fields (field prediction) */
static void vectors(struct pb *p, int s, int field) {
  const struct mx_mpeg *m = p->m;
  if (!field) {
    p->pmv[0][s][0] = vector(p, p->pmv[0][s][0], m->f_code[s][0]);
    p->pmv[0][s][1] = vector(p, p->pmv[0][s][1], m->f_code[s][1]);
    p->pmv[1][s][0] = p->pmv[0][s][0];
    p->pmv[1][s][1] = p->pmv[0][s][1];
    return;
  }
  for (int r = 0; r < 2; r++) {
    p->sel[r][s] = (u8)bits(p, 1);
    p->pmv[r][s][0] = vector(p, p->pmv[r][s][0], m->f_code[s][0]);
    p->pmv[r][s][1] = vector(p, p->pmv[r][s][1] >> 1, m->f_code[s][1]) << 1;
  }
}

/* The reference's pixels for a block at (x, y) of plane c (0 Y, 1 Cb, 2 Cr)
   of the picture at ref, rows lines of them from every step-th line starting
   with line first + y * step (a frame: 1, 0; a field: 2, its parity), into
   t: lines of 32 bytes (Y; 16 for Cb, Cr), the macroblock's and its right
   neighbour's. Returns where (x, y) is in t. */
static const u8 *fetch(const struct mx_mpeg *m, const u8 *ref, int c, int x, int y, int rows, int step,
                       int first, u8 *t) {
  if (!c) {
    u32 col = (u32)x >> 4, next = col + 1 < m->mbw ? 384 : 0;
    for (int j = 0; j < rows; j++, t += 32) {
      u32 line = (u32)(first + (y + j) * step);
      const u64 *s = (const u64 *)(const void *)(ref + ((line >> 4) * m->mbw + col) * 384u + (line & 15) * 16);
      u64 *d = (u64 *)(void *)t;
      d[0] = s[0], d[1] = s[1];
      d[2] = s[next / 8], d[3] = s[next / 8 + 1];
    }
    return t - rows * 32 + (x & 15);
  }
  u32 col = (u32)x >> 3, next = col + 1 < m->mbw ? 384 : 0;
  for (int j = 0; j < rows; j++, t += 16) {
    u32 line = (u32)(first + (y + j) * step);
    const u64 *s =
        (const u64 *)(const void *)(ref + ((line >> 3) * m->mbw + col) * 384u + 256 + (u32)(c - 1) * 64 + (line & 7) * 8);
    u64 *d = (u64 *)(void *)t;
    d[0] = s[0], d[1] = s[next / 8];
  }
  return t - rows * 16 + (x & 7);
}

/* A block of w x h pixels (w 16 or 8) of plane c from the picture at ref,
   at (x, y) moved by (dx, dy) half pixels, to dst; avg: the mean with what
   dst holds (the second direction). The plane is width x lines (of the
   field, if step is 2); the block is kept inside it. */
static void predict(const struct mx_mpeg *m, u8 *dst, u32 dstride, const u8 *ref, int c, int width, int lines,
                    int step, int first, int x, int y, int w, int h, int dx, int dy, int avg) {
  u64 tmp[17 * 4];
  int hx = dx & 1, hy = dy & 1;
  x += dx >> 1;
  y += dy >> 1;
  if (x < 0)
    x = 0, hx = 0;
  if (x > width - w - hx)
    x = width - w, hx = 0;
  if (y < 0)
    y = 0, hy = 0;
  if (y > lines - h - hy)
    y = lines - h, hy = 0;
  u32 sstride = c ? 16 : 32;
  const u8 *s = fetch(m, ref, c, x, y, h + hy, step, first, (u8 *)tmp), *s2 = s + sstride;
  int mode = hx | hy << 1;
  for (int j = 0; j < h; j++, s += sstride, s2 += sstride, dst += dstride)
    for (int k = 0; k < w; k += 8) {
      u64 v = ld8(s + k);
      if (mode == 1)
        v = avg2(v, ld8(s + k + 1));
      else if (mode == 2)
        v = avg2(v, ld8(s2 + k));
      else if (mode == 3)
        v = avg4(v, ld8(s + k + 1), ld8(s2 + k), ld8(s2 + k + 1));
      if (avg)
        v = avg2(ld8(dst + k), v);
      st8(dst + k, v);
    }
}

/* The prediction of the macroblock at (col, row) from direction s with the
   vectors in pmv, frame or field prediction. */
static void motion(struct pb *p, u32 col, u32 row, int s, int field, int avg) {
  const struct mx_mpeg *m = p->m;
  int x = (int)col * 16, y = (int)row * 16, W = m->mbw * 16, H = m->mbh * 16;
  int full = m->full_pel[s];
  u8 *mb = p->cur + (row * m->mbw + col) * 384u;
  const u8 *ref = p->from[s];
  if (!field) {
    int dx = p->pmv[0][s][0] << full, dy = p->pmv[0][s][1] << full;
    predict(m, mb, 16, ref, 0, W, H, 1, 0, x, y, 16, 16, dx, dy, avg);
    predict(m, mb + 256, 8, ref, 1, W / 2, H / 2, 1, 0, x / 2, y / 2, 8, 8, dx / 2, dy / 2, avg);
    predict(m, mb + 320, 8, ref, 2, W / 2, H / 2, 1, 0, x / 2, y / 2, 8, 8, dx / 2, dy / 2, avg);
    return;
  }
  for (u32 r = 0; r < 2; r++) { /* the macroblock's top and bottom field */
    int dx = p->pmv[r][s][0], dy = p->pmv[r][s][1] >> 1, f = p->sel[r][s];
    predict(m, mb + r * 16, 32, ref, 0, W, H / 2, 2, f, x, y / 2, 16, 8, dx, dy, avg);
    predict(m, mb + 256 + r * 8, 16, ref, 1, W / 2, H / 4, 2, f, x / 2, y / 4, 8, 4, dx / 2, dy / 2, avg);
    predict(m, mb + 320 + r * 8, 16, ref, 2, W / 2, H / 4, 2, f, x / 2, y / 4, 8, 4, dx / 2, dy / 2, avg);
  }
}

typedef s16 s16x8 __attribute__((vector_size(16)));
typedef u8 u8x16 __attribute__((vector_size(16)));

/* BDEC's macroblock into the picture: an intra one as it is, another added
   to the prediction. Both are 384 samples in the same order, 16-byte
   aligned: sixteen at a time in the EE's 128-bit registers (the bytes
   widened to 16 bits, added, kept within 0 .. 255, packed again). */
static void blocks(struct pb *p, u32 col, u32 row, int intra) {
  const s16x8 *b = (const s16x8 *)(mx_is_spr(p->m->block) ? p->m->block : MX_UNCACHED(p->m->block));
  u8x16 *d = (u8x16 *)(void *)(p->cur + (row * p->m->mbw + col) * 384u);
  const u8x16 zero = {0};
  const s16x8 max = {255, 255, 255, 255, 255, 255, 255, 255};
  for (u32 i = 0; i < 24; i++) {
    s16x8 lo = b[i * 2], hi = b[i * 2 + 1];
    if (!intra) {
      u8x16 px = d[i];
      lo += (s16x8)__builtin_shufflevector(px, zero, 0, 16, 1, 17, 2, 18, 3, 19, 4, 20, 5, 21, 6, 22, 7, 23);
      hi += (s16x8)__builtin_shufflevector(px, zero, 8, 24, 9, 25, 10, 26, 11, 27, 12, 28, 13, 29, 14, 30,
                                           15, 31);
    }
    lo &= lo > (s16x8){0};
    hi &= hi > (s16x8){0};
    s16x8 ml = lo > max, mh = hi > max;
    lo = (lo & ~ml) | (max & ml);
    hi = (hi & ~mh) | (max & mh);
    d[i] = __builtin_shufflevector((u8x16)lo, (u8x16)hi, 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26,
                                   28, 30);
  }
}

/* a macroblock passed over, or one with nothing coded: from the reference(s) */
static void copy_mb(struct pb *p, u32 col, u32 row) {
  int avg = 0;
  for (int s = 0; s < 2; s++)
    if (p->dirs & (s ? MB_BWD : MB_FWD)) {
      motion(p, col, row, s, p->field, avg);
      avg = 1;
    }
}

/* The slices of a picture, macroblock by macroblock. Returns MX_MPEG_MORE,
   or MX_MPEG_PICTURE with *pos after it and m->errors, m->decoded set. */
static int slices(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos, u32 cur) {
  struct pb p = {.m = m, .d = {buf, *pos >> 4, 0, size >> 4}};
  p.cur = frame(m, cur);
  p.from[0] = frame(m, m->type == MX_MPEG_B ? m->ref[0] : m->ref[1]);
  p.from[1] = frame(m, m->ref[1]);
  u32 n = p.d.endq - p.d.q0;
  if (n > FEED_MAX)
    n = FEED_MAX;
  p.d.fedq = p.d.q0 + n;
  mx_ipu_input(buf + p.d.q0 * 16, n, (*pos & 15) * 8);
  int frame_only = !m->mpeg2 || m->frame_dct; /* no frame_motion_type, no dct_type */
  u32 code = 0x100u | buf[*pos + 3], mbs = (u32)m->mbw * m->mbh;
  m->decoded = m->errors = 0;

  while (is_slice(code) && !p.starved) {
    u32 row = (code & 0xFF) - 1, addr = row * m->mbw, first = 1;
    int dc_reset = 1;
    p.valid = 0, p.used = 32; /* past the start code */
    p.qsc = bits(&p, 5);
    while (bits(&p, 1)) /* extra_information_slice */
      bits(&p, 8);
    memset(p.pmv, 0, sizeof p.pmv);
    p.dirs = MB_FWD, p.field = 0;
    p.bad = row >= m->mbh;
    while (!p.bad && !p.starved) {
      /* the address: macroblocks passed over come from the reference */
      u32 inc = 0, v;
      while ((v = vlc(&p, MX_IPU_VLC_MBAI) & 0xFFFF) >= 0x22)
        if (v == 0x23)
          inc += 33; /* macroblock_escape; 0x22: MPEG-1's stuffing */
      if (!v || p.starved) {
        p.bad = !p.starved;
        break;
      }
      inc += v;
      if (first) {
        addr += inc - 1; /* the slice's column */
        first = 0;
      } else if (inc > 1) {
        if (m->type == MX_MPEG_P) {
          memset(p.pmv, 0, sizeof p.pmv);
          p.dirs = MB_FWD, p.field = 0;
        }
        for (u32 k = 1; k < inc && addr < mbs; k++, addr++, m->decoded++)
          copy_mb(&p, addr % m->mbw, addr / m->mbw);
        dc_reset = 1;
      }
      if (addr >= mbs) {
        p.bad = 1;
        break;
      }
      u32 col = addr % m->mbw, r = addr / m->mbw;
      u32 t = vlc(&p, MX_IPU_VLC_MBT) & 0x1F;
      if (!t) {
        p.bad = !p.starved;
        break;
      }
      int field = 0, dct_field = 0;
      if (!frame_only) {
        if (t & (MB_FWD | MB_BWD)) {
          u32 mt = bits(&p, 2); /* frame_motion_type: 1 field, 2 frame, 3 dual prime */
          field = mt == 1;
          if (mt != 1 && mt != 2) {
            p.bad = 1;
            break;
          }
        }
        if (t & (MB_INTRA | MB_PATTERN))
          dct_field = (int)bits(&p, 1);
      }
      if (t & MB_QUANT)
        p.qsc = bits(&p, 5);
      if (t & MB_INTRA) {
        memset(p.pmv, 0, sizeof p.pmv);
        p.dirs = MB_FWD, p.field = 0; /* nothing may be passed over after it in a B picture */
      } else {
        if (m->type == MX_MPEG_P && !(t & MB_FWD)) {
          memset(p.pmv, 0, sizeof p.pmv); /* no vector: the same place */
          field = 0;
        }
        if (t & MB_FWD)
          vectors(&p, 0, field);
        if (t & MB_BWD)
          vectors(&p, 1, field);
        p.dirs = (u8)(m->type == MX_MPEG_P ? MB_FWD : t & (MB_FWD | MB_BWD));
        p.field = (u8)field;
        if (p.bad || p.starved)
          break;
      }
      /* the IPU decodes the blocks while the EE makes the prediction */
      int coded = (t & (MB_INTRA | MB_PATTERN)) != 0;
      if (coded) {
        u32 fl = (t & MB_INTRA ? MX_IPU_BDEC_INTRA | (dc_reset ? MX_IPU_BDEC_DCR : 0) : 0) |
                 (dct_field ? MX_IPU_BDEC_FIELD : 0);
        mx_ipu_bdec_start(p.valid ? p.used : 0, p.qsc, fl, m->block);
        p.valid = 0, p.used = 0;
      }
      if (!(t & MB_INTRA))
        copy_mb(&p, col, r);
      if (coded) {
        int got = mx_ipu_wait();
        while (got == MX_IPU_STARVED) {
          if (!more(&p.d)) {
            p.starved = 1;
            break;
          }
          got = mx_ipu_wait();
        }
        if (p.starved)
          break;
        if (got != 1 || MX_GET(IPU_CTRL_ECD, mx_ipu_status())) {
          p.bad = 1;
          break;
        }
        blocks(&p, col, r, t & MB_INTRA);
      }
      dc_reset = !(t & MB_INTRA);
      /* after a frame-predicted macroblock in a B picture one passed over
         uses its vectors; after a field-predicted one it may not be */
      addr++;
      m->decoded++;
      /* the slice's end: 23 zero bits and more before the next start code */
      if (peek(&p) >> 9 == 0)
        break;
    }
    if (p.starved)
      break;
    if (p.bad)
      m->errors++;
    /* the next start code: found on the EE from where the IPU is */
    u32 at = next_code(buf, p.d.q0 * 16 + (mx_ipu_pos() + (p.valid ? p.used : 0)) / 8, size);
    if (at + 4 + MX_MPEG_AHEAD > size) {
      p.starved = 1;
      break;
    }
    code = 0x100u | buf[at + 3];
    *pos = at;
    if (!is_slice(code))
      break;
    /* on from there (the IPU's own place is within a few bytes of it) */
    p.d.q0 = at >> 4;
    n = p.d.endq - p.d.q0;
    if (n > FEED_MAX)
      n = FEED_MAX;
    p.d.fedq = p.d.q0 + n;
    mx_ipu_input(buf + p.d.q0 * 16, n, (at & 15) * 8);
  }
  mx_ipu_input_end();
  return p.starved ? MX_MPEG_MORE : MX_MPEG_PICTURE;
}

/* A picture with mx_mpeg_refs: decoded to Y, Cb, Cr, and the one whose turn
   it is converted to out. */
static int picture_refs(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos, void *out) {
  int b = m->type == MX_MPEG_B;
  /* what is not decoded: field pictures, concealment vectors, other chroma;
     a P picture without its reference, a B picture without both */
  if (m->type < MX_MPEG_I || m->type > MX_MPEG_B || m->structure != 3 || m->chroma != 1 || m->conceal ||
      m->nrefs < (b ? 2 : m->type == MX_MPEG_P ? 1 : 0))
    return skip_slices(buf, size, pos);

  for (int n = 0; n < 2; n++)
    if (m->iq_load[n]) {
      mx_ipu_setiq_mem(n, m->iq[n]);
      m->iq_load[n] = 0;
    }
  mx_ipu_ctrl((u32)(MX_SET(IPU_CTRL_IDP, m->dc_prec) | MX_SET(IPU_CTRL_AS, m->alt_scan) |
                    MX_SET(IPU_CTRL_IVF, m->vlc_fmt) | MX_SET(IPU_CTRL_QST, m->q_type) |
                    MX_SET(IPU_CTRL_MP1, !m->mpeg2) | MX_SET(IPU_CTRL_PCT, m->type)));
  /* a reference picture takes the place of the older one, which the newer
     one's predictions no longer need... but a P picture is predicted from
     the newer, so it goes into the spare frame and the frames turn after */
  u32 cur = m->spare;
  u32 start = *pos;
  mx_ipu_direct(1);
  int r = slices(m, buf, size, pos, cur);
  mx_ipu_direct(0);
  if (r == MX_MPEG_MORE) {
    *pos = start;
    mx_ipu_reset();
    m->iq_load[0] = m->iq_load[1] = 1;
    return r;
  }
  if (b) {
    show(m, cur, out);
    return MX_MPEG_PICTURE;
  }
  /* the newer reference is shown now that the one after it is decoded */
  int had = m->held;
  if (had)
    show(m, m->ref[1], out);
  u8 old = m->ref[0];
  m->ref[0] = m->ref[1];
  m->ref[1] = (u8)cur;
  m->spare = old;
  if (m->nrefs < 2)
    m->nrefs++;
  m->held = 1;
  return had ? MX_MPEG_PICTURE : MX_MPEG_NONE;
}

int mx_mpeg_picture(struct mx_mpeg *m, const u8 *buf, u32 size, u32 *pos, void *out) {
  if (!m->have_seq)
    return MX_MPEG_BAD;
  if (m->frames)
    return picture_refs(m, buf, size, pos, out);
  /* what the IPU alone decodes: an intra frame picture in 4:2:0 without
     concealment vectors, a row of macroblocks within a transfer */
  if (m->type != MX_MPEG_I || m->structure != 3 || m->chroma != 1 || m->conceal || m->mbw > 1023)
    return skip_slices(buf, size, pos);

  for (int n = 0; n < 2; n++)
    if (m->iq_load[n]) {
      mx_ipu_setiq_mem(n, m->iq[n]);
      m->iq_load[n] = 0;
    }
  mx_ipu_ctrl((u32)(MX_SET(IPU_CTRL_IDP, m->dc_prec) | MX_SET(IPU_CTRL_AS, m->alt_scan) |
                    MX_SET(IPU_CTRL_IVF, m->vlc_fmt) | MX_SET(IPU_CTRL_QST, m->q_type) |
                    MX_SET(IPU_CTRL_MP1, !m->mpeg2) | MX_SET(IPU_CTRL_PCT, MX_MPEG_I)));

  u32 mbq = m->flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32;
  u32 flags = m->flags | (m->mpeg2 && !m->frame_dct ? MX_IPU_IDEC_DTD : 0);
  struct dec d = {buf, *pos >> 4, 0, size >> 4};
  u32 n = d.endq - d.q0;
  if (n > FEED_MAX)
    n = FEED_MAX;
  d.fedq = d.q0 + n;
  mx_ipu_input(buf + d.q0 * 16, n, (*pos & 15) * 8);

  int starved = 0;
  m->decoded = m->errors = 0;
  u32 code = 0x100u | buf[*pos + 3]; /* mx_mpeg_headers saw it */
  while (is_slice(code)) {
    u32 row = (code & 0xFF) - 1, col = 0, fb = 0;
    /* quantiser_scale_code (5), extra_bit_slice (0), then the first
       macroblock's address increment: '1' for column 0 */
    u32 v = fdec(&d, 32, &starved);
    if (starved)
      break;
    int bad = (v >> 26 & 1) || row >= m->mbh;
    if (v >> 25 & 1) {
      fb = 7;
    } else if (!bad) {
      u32 inc = mx_ipu_vdec(MX_IPU_VLC_MBAI, 6);
      if (busy()) { /* the transfer's end inside the code */
        if (!more(&d)) {
          starved = 1;
          break;
        }
        mx_ipu_wait();
        inc = 0; /* the slice is given up */
      }
      col = (inc & 0xFFFF) - 1;
      bad = !inc || col >= m->mbw;
      fb = 0;
    }
    if (!bad) {
      int r = mx_ipu_idec(fb, v >> 27, flags, (u8 *)out + (row * m->mbw + col) * mbq * 16, m->mbw - col);
      while (r == MX_IPU_STARVED) {
        if (!more(&d)) {
          starved = 1;
          break;
        }
        r = mx_ipu_wait();
      }
      if (starved)
        break;
      if (r > 0)
        m->decoded += (u16)r;
      u32 st = mx_ipu_status();
      bad = r < 0 || MX_GET(IPU_CTRL_ECD, st) || !MX_GET(IPU_CTRL_SCD, st);
      if (r == MX_IPU_FULL) { /* the IPU was reset */
        m->iq_load[0] = m->iq_load[1] = 1;
        m->errors++;
        return skip_slices(buf, size, pos) == MX_MPEG_MORE ? MX_MPEG_MORE : MX_MPEG_PICTURE;
      }
    }
    if (bad) {
      /* on from the next start code, found on the EE */
      m->errors++;
      u32 p = next_code(buf, d.q0 * 16 + mx_ipu_pos() / 8 + 1, size);
      if (p + 4 + MX_MPEG_AHEAD > size) {
        starved = 1;
        break;
      }
      d.q0 = p >> 4;
      n = d.endq - d.q0;
      if (n > FEED_MAX)
        n = FEED_MAX;
      d.fedq = d.q0 + n;
      mx_ipu_input(buf + d.q0 * 16, n, (p & 15) * 8);
      code = 0x100u | buf[p + 3];
      continue;
    }
    code = mx_ipu_top();
  }
  if (starved) {
    mx_ipu_reset();
    m->iq_load[0] = m->iq_load[1] = 1;
    return MX_MPEG_MORE;
  }
  *pos = d.q0 * 16 + mx_ipu_pos() / 8;
  mx_ipu_input_end(); /* the channel would go on filling the FIFO with what follows */
  return MX_MPEG_PICTURE;
}

/* ---- to lines of pixels ------------------------------------------------------- */

void mx_mpeg_raster(const struct mx_mpeg *m, const void *mbs, void *out, u32 stride) {
  u32 px = m->flags & MX_IPU_RGB16 ? 2 : 4, mb = 256 * px, row = 16 * px;
  mx_dcache_invalidate(mbs, (u32)m->mbw * m->mbh * mb); /* the DMAC wrote it */
  for (u32 r = 0; r < m->mbh; r++)
    for (u32 c = 0; c < m->mbw; c++) {
      const u8 *s = (const u8 *)mbs + (r * m->mbw + c) * mb;
      u8 *d = (u8 *)out + r * 16 * stride + c * row;
      for (u32 y = 0; y < 16; y++, s += row, d += stride)
        memcpy(d, s, row);
    }
}

/* ---- to GS memory ------------------------------------------------------------ */

u32 mx_mpeg_chain(u64 *tags, const struct mx_mpeg *m, const void *mbs, u32 dbp, u32 dbw, u32 psm,
                  u32 x, u32 y, u32 end_id) {
  u32 mbq = m->flags & MX_IPU_RGB16 ? MX_IPU_MB_RGB16 : MX_IPU_MB_RGB32;
  u64 *c = tags;
  for (u32 i = 0; i < (u32)m->mbw * m->mbh; i++) {
    u32 regs = i ? 2 : 4;
    *c++ = mx_dmatag(MX_TAG_CNT, regs + 2, MX_PADDR_NONE, 0);
    *c++ = 0;
    *c++ = mx_giftag(regs, 0, -1, MX_GIF_FLG_PACKED, 1);
    *c++ = MX_GIF_REG_AD;
    if (!i) {
      *c++ = mx_gs_bitbltbuf(0, 0, 0, dbp, dbw, psm);
      *c++ = MX_GS_REG_BITBLTBUF;
      *c++ = mx_gs_trxreg(16, 16);
      *c++ = MX_GS_REG_TRXREG;
    }
    *c++ = mx_gs_trxpos(0, 0, x + (i % m->mbw) * 16, y + (i / m->mbw) * 16, 0);
    *c++ = MX_GS_REG_TRXPOS;
    *c++ = 0;
    *c++ = MX_GS_REG_TRXDIR;
    *c++ = mx_giftag(mbq, 1, -1, MX_GIF_FLG_IMAGE, 0);
    *c++ = 0;
    *c++ = mx_dmatag(MX_TAG_REF, mbq, mx_paddr_range((const u8 *)mbs + i * mbq * 16, mbq * 16), 0);
    *c++ = 0;
  }
  /* the texture cache must not keep the old picture */
  *c++ = mx_dmatag(end_id, 2, MX_PADDR_NONE, 0);
  *c++ = 0;
  *c++ = mx_giftag(1, 1, -1, MX_GIF_FLG_PACKED, 1);
  *c++ = MX_GIF_REG_AD;
  *c++ = 0;
  *c++ = MX_GS_REG_TEXFLUSH;
  mx_dcache_writeback(tags, (u32)((u8 *)c - (u8 *)tags));
  return (u32)(c - tags) / 2;
}
