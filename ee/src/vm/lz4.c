/* LZ4 block format for the VM's pages (vm.c: compressed swap, mxswap.irx).
   A greedy compressor with a hash of 4 bytes and word-wide match search,
   and a decompressor that copies 8 bytes at a time; both for one page of
   at most 64 KiB (positions in 16 bits). The format is LZ4's block format,
   so a page can be checked with lz4 on the PC. */
#include <mx/types.h>

#include "lz4.h"

#define HBITS 12
static u16 htab[1 << HBITS];

static inline u32 ld32(const u8 *p) {
  u32 v;
  __builtin_memcpy(&v, p, 4);
  return v;
}
static inline u64 ld64(const u8 *p) {
  u64 v;
  __builtin_memcpy(&v, p, 8);
  return v;
}
static inline void st64(u8 *p, u64 v) { __builtin_memcpy(p, &v, 8); }
static inline u32 hash(u32 v) { return (v * 2654435761u) >> (32 - HBITS); }

static u8 *len_out(u8 *o, u32 n) {
  while (n >= 255)
    *o++ = 255, n -= 255;
  *o++ = (u8)n;
  return o;
}

/* the literals [a, a + n) to o: 8 at a time (the output has room for it) */
static inline u8 *lit_copy(u8 *o, const u8 *a, u32 n) {
  for (u32 i = 0; i < n; i += 8)
    st64(o + i, ld64(a + i));
  return o + n;
}

u32 mx__lz4_compress(const u8 *in, u32 n, u8 *out, u32 room) {
  for (u32 i = 0; i < (1u << HBITS); i++)
    htab[i] = 0;
  u32 i = 1, anchor = 0;
  u8 *o = out, *end = out + room;
  htab[hash(ld32(in))] = 0;
  /* the last 12 bytes are literals (LZ4's end rules: the last match ends 5
     bytes before the end, the last 5 bytes are literals) */
  u32 misses = 0;
  while (i + 12 < n) {
    u32 v = ld32(in + i), h = hash(v), cand = htab[h];
    htab[h] = (u16)i;
    if (ld32(in + cand) != v || i - cand > 65535 || cand >= i) {
      /* LZ4's acceleration: longer steps where nothing matches (data that
         does not compress costs less) */
      i += 1 + (misses++ >> 5);
      continue;
    }
    misses = 0;
    u32 m = 4, lim = n - 5 - i;
    while (m + 8 <= lim && ld64(in + cand + m) == ld64(in + i + m))
      m += 8;
    while (m < lim && in[cand + m] == in[i + m])
      m++;
    u32 lit = i - anchor;
    if (o + 1 + lit / 255 + lit + 8 + 2 + m / 255 + 1 > end)
      return 0; /* does not fit */
    u8 *tok = o++;
    *tok = (u8)((lit < 15 ? lit : 15) << 4 | (m - 4 < 15 ? m - 4 : 15));
    if (lit >= 15)
      o = len_out(o, lit - 15);
    o = lit_copy(o, in + anchor, lit);
    u32 off = i - cand;
    *o++ = (u8)off, *o++ = (u8)(off >> 8);
    if (m - 4 >= 15)
      o = len_out(o, m - 4 - 15);
    /* the positions inside the match: two of them, for the next ones */
    if (i + m + 12 < n)
      htab[hash(ld32(in + i + m - 2))] = (u16)(i + m - 2);
    i += m;
    anchor = i;
  }
  u32 lit = n - anchor;
  if (o + 1 + lit / 255 + lit + 1 > end)
    return 0;
  *o++ = (u8)((lit < 15 ? lit : 15) << 4);
  if (lit >= 15)
    o = len_out(o, lit - 15);
  for (u32 k = 0; k < lit; k++)
    o[k] = in[anchor + k];
  return (u32)(o + lit - out);
}

u32 mx__lz4_decompress(const u8 *in, u32 n, u8 *out, u32 room) {
  const u8 *e = in + n;
  u8 *o = out, *oe = out + room;
  while (in < e) {
    u32 t = *in++, lit = t >> 4;
    if (lit == 15) {
      u32 b;
      do
        lit += b = *in++;
      while (b == 255 && in < e);
    }
    if (o + lit > oe || in + lit > e)
      return 0;
    if (o + lit + 8 <= oe && in + lit + 8 <= e)
      lit_copy(o, in, lit);
    else
      for (u32 k = 0; k < lit; k++)
        o[k] = in[k];
    o += lit, in += lit;
    if (in >= e)
      break;
    u32 off = in[0] | in[1] << 8, m = (t & 15) + 4;
    in += 2;
    if ((t & 15) == 15) {
      u32 b;
      do
        m += b = *in++;
      while (b == 255 && in < e);
    }
    if (!off || off > (u32)(o - out) || o + m > oe)
      return 0;
    const u8 *s = o - off;
    if (off >= 8 && o + m + 8 <= oe) {
      for (u32 k = 0; k < m; k += 8)
        st64(o + k, ld64(s + k));
      o += m;
    } else
      while (m--)
        *o++ = *s++;
  }
  return (u32)(o - out);
}
