/* PNG (ISO 15948) on the EE (<mx/pic.h>): the IDAT chunks' zlib stream
   inflated (RFC 1950, 1951) into the picture's filtered lines, the lines'
   filters undone, the samples made pixels. Not interlaced pictures. */

#include <mx/mem.h>
#include <mx/pic.h>
#include <mx/string.h>

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* ---- inflate ------------------------------------------------------------------ */

struct inflate {
  const u8 *p, *end;  /* the IDAT chunk being read */
  const u8 *file_end; /* and the file, for the chunks after it */
  u32 buf;            /* bits not taken yet, from bit 0 up */
  u32 bits;
  u8 *out;
  u32 have, size;
  int bad;
};

/* the next byte of the IDAT chunks' data: on to the next chunk at one's end
   (4 bytes of CRC, the length, the name) */
static u32 byte(struct inflate *s) {
  while (s->p == s->end) {
    const u8 *c = s->end + 4;
    if (c + 8 > s->file_end || memcmp(c + 4, "IDAT", 4) || c + 8 + be32(c) > s->file_end) {
      s->bad = 1;
      return 0;
    }
    s->p = c + 8;
    s->end = s->p + be32(c);
  }
  return *s->p++;
}

static u32 bits(struct inflate *s, u32 n) {
  while (s->bits < n) {
    s->buf |= byte(s) << s->bits;
    s->bits += 8;
  }
  u32 v = s->buf & ((1u << n) - 1);
  s->buf >>= n;
  s->bits -= n;
  return v;
}

/* A Huffman code: by the stream's next 9 bits, the symbol and its length for
   the codes that short (most of what a stream holds); and the lengths'
   counts with the symbols in the codes' order, for the canonical way a bit
   at a time (the codes of a length follow each other, the first one twice
   the last of the length before plus 1). */
struct code {
  u16 fast[512]; /* symbol | length << 12; 0: a longer code */
  u16 count[16];
  u16 symbol[288];
};

static void build(struct code *c, const u8 *len, u32 n) {
  u16 at[16], next[16];
  memset(c->count, 0, sizeof c->count);
  memset(c->fast, 0, sizeof c->fast);
  for (u32 i = 0; i < n; i++)
    c->count[len[i]]++;
  c->count[0] = 0;
  at[1] = 0, next[1] = 0;
  for (u32 l = 1; l < 15; l++) {
    at[l + 1] = (u16)(at[l] + c->count[l]);
    next[l + 1] = (u16)((next[l] + c->count[l]) << 1);
  }
  for (u32 i = 0; i < n; i++) {
    u32 l = len[i];
    if (!l)
      continue;
    c->symbol[at[l]++] = (u16)i;
    u32 code = next[l]++;
    if (l <= 9) {
      /* the stream has a code's first bit lowest */
      u32 rev = 0;
      for (u32 k = 0; k < l; k++)
        rev |= (code >> k & 1) << (l - 1 - k);
      for (u32 f = rev; f < 512; f += 1u << l)
        c->fast[f] = (u16)(i | l << 12);
    }
  }
}

static u32 symbol(struct inflate *s, const struct code *c) {
  while (s->bits < 9) { /* the stream's end has four bytes more than its codes */
    s->buf |= byte(s) << s->bits;
    s->bits += 8;
  }
  u32 e = c->fast[s->buf & 511];
  if (e) {
    s->buf >>= e >> 12;
    s->bits -= e >> 12;
    return e & 0xFFF;
  }
  u32 code = 0, first = 0, index = 0;
  for (u32 l = 1; l < 16; l++) {
    code |= bits(s, 1);
    u32 n = c->count[l];
    if (code - first < n)
      return c->symbol[index + code - first];
    index += n;
    first = (first + n) << 1;
    code <<= 1;
  }
  s->bad = 1;
  return 0;
}

static const u16 len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const u8 len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const u16 dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const u8 dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                  6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* a block's codes: literals and lengths with lc, distances with dc */
static void codes(struct inflate *s, const struct code *lc, const struct code *dc) {
  while (!s->bad) {
    u32 sym = symbol(s, lc);
    if (sym < 256) {
      if (s->have >= s->size)
        s->bad = 1;
      else
        s->out[s->have++] = (u8)sym;
      continue;
    }
    if (sym == 256)
      return;
    sym -= 257;
    if (sym >= 29) {
      s->bad = 1;
      return;
    }
    u32 len = len_base[sym] + bits(s, len_extra[sym]);
    sym = symbol(s, dc);
    if (sym >= 30) {
      s->bad = 1;
      return;
    }
    u32 dist = dist_base[sym] + bits(s, dist_extra[sym]);
    if (dist > s->have || s->have + len > s->size) {
      s->bad = 1;
      return;
    }
    for (u8 *d = s->out + s->have, *e = d + len; d < e; d++)
      *d = d[-(s32)dist];
    s->have += len;
  }
}

static void inflate(struct inflate *s) {
  static struct code lc, dc; /* large: not on the stack */
  u8 len[320];
  bits(s, 16); /* zlib's header: the method and the window, taken as they are */
  for (int last = 0; !last && !s->bad;) {
    last = (int)bits(s, 1);
    u32 type = bits(s, 2);
    if (type == 0) { /* stored: whole bytes, their count and its complement */
      s->buf = 0, s->bits = 0;
      u32 n = byte(s);
      n |= byte(s) << 8;
      byte(s), byte(s);
      if (s->have + n > s->size)
        s->bad = 1;
      for (u32 i = 0; i < n && !s->bad; i++)
        s->out[s->have++] = (u8)byte(s);
    } else if (type == 1) { /* the fixed codes */
      for (u32 i = 0; i < 288; i++)
        len[i] = (u8)(i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8);
      build(&lc, len, 288);
      memset(len, 5, 30);
      build(&dc, len, 30);
      codes(s, &lc, &dc);
    } else if (type == 2) { /* codes of the block's own, their lengths coded too */
      static const u8 order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
      u32 nl = bits(s, 5) + 257, nd = bits(s, 5) + 1, nc = bits(s, 4) + 4;
      memset(len, 0, 19);
      for (u32 i = 0; i < nc; i++)
        len[order[i]] = (u8)bits(s, 3);
      build(&lc, len, 19);
      for (u32 i = 0; i < nl + nd && !s->bad;) {
        u32 sym = symbol(s, &lc), rep = 1, v = sym;
        if (sym == 16) {
          if (!i) {
            s->bad = 1;
            break;
          }
          v = len[i - 1], rep = 3 + bits(s, 2);
        } else if (sym == 17) {
          v = 0, rep = 3 + bits(s, 3);
        } else if (sym == 18) {
          v = 0, rep = 11 + bits(s, 7);
        }
        if (i + rep > nl + nd) {
          s->bad = 1;
          break;
        }
        while (rep--)
          len[i++] = (u8)v;
      }
      if (!s->bad) {
        build(&lc, len, nl);
        build(&dc, len + nl, nd);
        codes(s, &lc, &dc);
      }
    } else {
      s->bad = 1;
    }
  }
}

/* ---- the picture ---------------------------------------------------------------- */

struct png {
  u32 w, h;
  u8 depth, type, interlace;
  u8 channels;
  const u8 *plte, *trns, *idat;
  u32 nplte, ntrns, idat_len;
};

/* the chunks before the picture's data */
static int chunks(const u8 *d, u32 bytes, struct png *g) {
  static const u8 sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  memset(g, 0, sizeof *g);
  if (bytes < 8 + 25 || memcmp(d, sig, 8) || memcmp(d + 12, "IHDR", 4))
    return MX_PIC_FORMAT;
  g->w = be32(d + 16), g->h = be32(d + 20);
  g->depth = d[24], g->type = d[25], g->interlace = d[28];
  static const u8 ch[7] = {1, 0, 3, 1, 2, 0, 4};
  g->channels = g->type < 7 ? ch[g->type] : 0;
  if (!g->channels || g->interlace || g->w > 4096 || g->h > 4096 ||
      (g->depth != 8 && g->depth != 16 && (g->type == 2 || g->type >= 4)) ||
      (g->depth != 1 && g->depth != 2 && g->depth != 4 && g->depth != 8 && g->depth != 16) ||
      (g->type == 3 && g->depth == 16))
    return MX_PIC_FORMAT;
  for (u32 at = 8; at + 12 <= bytes;) {
    u32 len = be32(d + at);
    const u8 *name = d + at + 4, *data = d + at + 8;
    if (len > bytes - at - 12)
      return MX_PIC_DATA;
    if (!memcmp(name, "PLTE", 4))
      g->plte = data, g->nplte = len / 3;
    else if (!memcmp(name, "tRNS", 4))
      g->trns = data, g->ntrns = len;
    else if (!memcmp(name, "IDAT", 4)) {
      g->idat = data, g->idat_len = len;
      return g->type == 3 && !g->plte ? MX_PIC_DATA : MX_PIC_OK;
    }
    at += 12 + len;
  }
  return MX_PIC_DATA;
}

int mx_png_size(const void *data, u32 bytes, u16 *width, u16 *height) {
  struct png g;
  int r = chunks(data, bytes, &g);
  *width = (u16)g.w, *height = (u16)g.h;
  return r;
}

static int paeth(int a, int b, int c) {
  int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* alpha as the GS has it: 0x80 is opaque */
static u32 alpha_of(u32 a) { return (a + 1) >> 1; }

int mx_png_decode(const void *data, u32 bytes, u32 *out, u8 *alpha) {
  struct png g;
  int r = chunks(data, bytes, &g);
  if (r)
    return r;
  u32 bpp = (g.channels * g.depth + 7u) / 8, line = (g.w * g.channels * g.depth + 7) / 8;
  u32 raw_bytes = (line + 1) * g.h;
  u8 *raw = mx_heap_alloc(mx_heap_paged, raw_bytes, 64);
  if (!raw)
    return MX_PIC_MEMORY;
  struct inflate s = {.p = g.idat, .end = g.idat + g.idat_len, .file_end = (const u8 *)data + bytes,
                      .out = raw, .size = raw_bytes};
  inflate(&s);
  if (s.have != raw_bytes) { /* the lines must all be there */
    mx_heap_free(mx_heap_paged, raw);
    return MX_PIC_DATA;
  }
  *alpha = 0;
  for (u32 y = 0; y < g.h; y++) {
    u8 *l = raw + y * (line + 1) + 1, *up = y ? l - (line + 1) : 0;
    u32 f = l[-1];
    /* the line's filter undone: each byte from the one a pixel before, the
       one above, the one before that */
    for (u32 i = 0; i < line; i++) {
      int a = i >= bpp ? l[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= bpp ? up[i - bpp] : 0;
      int v = l[i];
      v += f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : f == 4 ? paeth(a, b, c) : 0;
      l[i] = (u8)v;
    }
    u32 *o = out + y * g.w;
    for (u32 x = 0; x < g.w; x++) {
      u32 red, green, blue, a = 255;
      if (g.depth >= 8) {
        const u8 *p = l + x * bpp;
        u32 step = g.depth / 8; /* 16 bits: the high byte */
        if (g.type == 3) {
          u32 i = p[0];
          const u8 *c = g.plte + (i < g.nplte ? i : 0) * 3;
          red = c[0], green = c[1], blue = c[2];
          if (i < g.ntrns)
            a = g.trns[i];
        } else if (g.type == 0 || g.type == 4) {
          red = green = blue = p[0];
          if (g.type == 4)
            a = p[step];
          else if (g.ntrns >= 2 && p[0] == g.trns[step == 2 ? 0 : 1] && (step == 1 || p[1] == g.trns[1]))
            a = 0;
        } else {
          red = p[0], green = p[step], blue = p[2 * step];
          if (g.type == 6)
            a = p[3 * step];
          else if (g.ntrns >= 6 && step == 1 && red == g.trns[1] && green == g.trns[3] && blue == g.trns[5])
            a = 0;
        }
      } else { /* 1, 2 or 4 bits: grey or palette, the first pixel in the high bits */
        u32 v = (u32)(l[x * g.depth / 8] >> (8 - g.depth - (x * g.depth & 7))) & ((1u << g.depth) - 1);
        if (g.type == 3) {
          const u8 *c = g.plte + (v < g.nplte ? v : 0) * 3;
          red = c[0], green = c[1], blue = c[2];
          if (v < g.ntrns)
            a = g.trns[v];
        } else {
          red = green = blue = v * 255 / ((1u << g.depth) - 1);
          if (g.ntrns >= 2 && v == g.trns[1])
            a = 0;
        }
      }
      if (a != 255)
        *alpha = 1;
      o[x] = alpha_of(a) << 24 | blue << 16 | green << 8 | red;
    }
  }
  mx_heap_free(mx_heap_paged, raw);
  return MX_PIC_OK;
}
