/* GS memory by pages and the cache in it (<mx/vram.h>, <mx/vcache.h>) on
   the GS: (a) the page map under mx_gs_init's buffers; (b) 48 textures of
   CT32, CT16, T8 and T4 (MIPMAP levels, CLUTs) through a cache of 12 pages
   over 40 frames, with each policy (AUTO last: (c) uses what it left), every entry read back after each frame and compared
   with what was uploaded for it (small pieces of other formats share its
   pages); (c) textures drawn from their places (sub-page TBPs, CLUTs); (d)
   MX_VCACHE_STREAM's premise: in one stream, a texture drawn and then
   overwritten by the next one's upload is drawn as it was. */

#include "t.h"
#include <mx/vcache.h>

static u32 pk[4096] MX_ALIGNED(64);
static u8 data[32768] MX_ALIGNED(64);
static u8 back[32768] MX_ALIGNED(64);
static u32 px[64] MX_ALIGNED(64);

static void send(struct mx_pkt *p) {
  mx_pkt_end(p, 0, 0);
  CHECK(mx_pkt_finish(p) > 0);
  mx_pkt_writeback(p);
  u32 f0 = mx_gs.finish;
  mx_gs_send(mx_pkt_start(p));
  CHECK(mx_gs_wait_finish(f0, 1) == 0);
}

/* ---- the textures ---------------------------------------------------------- */

#define NKEYS 48
struct shape {
  u32 psm, tw, th, levels;
};
static const struct shape shapes[6] = {
    {MX_GS_PSM_CT32, 6, 6, 4}, /* 64 x 64 .. 8 x 8: level 0 two pages */
    {MX_GS_PSM_CT32, 5, 5, 3}, /* 32 x 32 .. 8 x 8 */
    {MX_GS_PSM_CT16, 6, 5, 3}, /* 64 x 32 .. 16 x 8 */
    {MX_GS_PSM_T8, 6, 6, 3},   /* 64 x 64 .. 16 x 16, CLUT 16 x 16 */
    {MX_GS_PSM_T4, 6, 5, 2},   /* 64 x 32, 32 x 16, CLUT 8 x 2 */
    {MX_GS_PSM_CT32, 7, 6, 1}, /* 128 x 64: four pages */
};
static const struct shape *shape_of(u32 k) { return &shapes[k % 6]; }
static int indexed(u32 psm) { return psm == MX_GS_PSM_T8 || psm == MX_GS_PSM_T4; }

/* bits a pixel */
static u32 bpp(u32 psm) {
  switch (psm) {
  case MX_GS_PSM_CT16:
    return 16;
  case MX_GS_PSM_T8:
    return 8;
  case MX_GS_PSM_T4:
    return 4;
  }
  return 32;
}

/* piece n of key k (n == levels: the CLUT): its pixels, format and bytes */
static void piece(u32 k, u32 n, u32 *w, u32 *h, u32 *psm) {
  const struct shape *s = shape_of(k);
  if (n == s->levels) {
    int t4 = s->psm == MX_GS_PSM_T4;
    *w = t4 ? 8 : 16, *h = t4 ? 2 : 16, *psm = MX_GS_PSM_CT32;
    return;
  }
  *w = 1u << (s->tw - n), *h = 1u << (s->th - n), *psm = s->psm;
}

/* the bytes of piece n of key k, generation g (a key uploaded again gets
   other bytes) */
static u8 byte_of(u32 k, u32 n, u32 g, u32 i) {
  u32 v = i * 2654435761u + k * 40503u + n * 977u + g * 7919u;
  return (u8)(v >> 13 ^ v >> 24);
}
static void make(u32 k, u32 n, u32 g, u8 *d, u32 bytes) {
  for (u32 i = 0; i < bytes; i++)
    d[i] = byte_of(k, n, g, i);
}

static u32 gen[NKEYS]; /* uploads of each key */

static void upload(u32 k, const struct mx_tex *t) {
  const struct shape *s = shape_of(k);
  gen[k]++;
  u32 pieces = s->levels + indexed(s->psm);
  for (u32 n = 0; n < pieces; n++) {
    u32 w, h, psm;
    piece(k, n, &w, &h, &psm);
    u32 bytes = w * h * bpp(psm) / 8;
    make(k, n, gen[k], data, bytes);
    mx_dcache_writeback(data, bytes);
    struct mx_pkt p;
    mx_pkt_begin(&p, pk, sizeof pk, 0);
    mx_pkt_cnt(&p, 0, 0);
    if (n == s->levels)
      mx_gs_upload(&p, t->cbp, 1, psm, 0, 0, w, h, data);
    else
      mx_gs_upload(&p, t->tbp[n], t->tbw[n], psm, 0, 0, w, h, data);
    mx_gs_finish_ad(&p);
    send(&p);
  }
}

/* GS memory can't be read back as T4 (GS manual 4.3: local -> host not
   with PSMT4/T4HL/T4HH): level n of a T4 key is drawn with a CLUT that
   gives each index itself (idclut), and the pixels compared */
static u32 idclut;
static void sprite(struct mx_pkt *p, const struct mx_tex *t, u32 x, u32 y);
static u32 verify_t4(u32 k, u32 n, u32 bp, u32 bw, u32 w, u32 h) {
  u32 lw = 0, lh = 0;
  while ((1u << lw) < w)
    lw++;
  while ((1u << lh) < h)
    lh++;
  struct mx_tex t = {.psm = MX_GS_PSM_T4, .tw = (u8)lw, .th = (u8)lh, .levels = 1,
                     .cbp = idclut, .cpsm = MX_GS_PSM_CT32};
  t.tbp[0] = bp, t.tbw[0] = (u8)bw;
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  sprite(&p, &t, 0, 150);
  mx_gs_finish_ad(&p);
  send(&p);
  u32 bad = 0;
  for (u32 y = 0; y < h; y++) {
    mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, 0, 150 + y, 64, 1);
    for (u32 x = 0; x < w; x++) {
      u32 i = y * w + x, want = 0x80000000u | (byte_of(k, n, gen[k], i / 2) >> (i & 1) * 4 & 15);
      if (px[x] != want) {
        if (!bad)
          mx_printf("key %u piece %u at %u: T4 texel (%u, %u) is %08x, expected %08x\n", k, n, bp,
                    x, y, px[x], want);
        bad++;
      }
    }
  }
  return bad;
}

/* every piece of key k as uploaded last; returns the bad bytes */
static u32 verify(u32 k, const struct mx_tex *t) {
  const struct shape *s = shape_of(k);
  u32 bad = 0, pieces = s->levels + indexed(s->psm);
  for (u32 n = 0; n < pieces; n++) {
    u32 w, h, psm, bp, bw;
    piece(k, n, &w, &h, &psm);
    bp = n == s->levels ? t->cbp : t->tbp[n];
    bw = n == s->levels ? 1 : t->tbw[n];
    u32 bytes = w * h * bpp(psm) / 8, rh = h;
    if (psm == MX_GS_PSM_T4) {
      bad += verify_t4(k, n, bp, bw, w, h);
      continue;
    }
    /* a readback is a multiple of 128 bytes: the 8 x 2 CLUT is read as
       8 x 4 (the rest of its block, the entry's own) */
    if (bytes < 128)
      rh = h * 128 / bytes;
    if (mx_gs_download(back, bp, bw, psm, 0, 0, w, rh) != 0) {
      mx_printf("key %u piece %u: download failed\n", k, n);
      return 1;
    }
    for (u32 i = 0; i < bytes; i++)
      if (back[i] != byte_of(k, n, gen[k], i)) {
        if (!bad)
          mx_printf("key %u piece %u at %u: byte %u is %02x, expected %02x\n", k, n, bp, i,
                    back[i], byte_of(k, n, gen[k], i));
        bad++;
      }
  }
  return bad;
}

/* (b) */
static void cache_load(int policy) {
  mx_vcache_init(12, MX_VCACHE_STREAM, policy);
  for (u32 k = 0; k < NKEYS; k++)
    gen[k] = 0;
  u32 s = 1, bad = 0, checked = 0, tmax = 0, tsum[2] = {0, 0}, nlook[2] = {0, 0};
  for (u32 f = 1; f <= 40; f++) {
    mx_vcache_frame(f, f - 1);
    for (u32 i = 0; i < 16; i++) {
      s = s * 1103515245u + 12345u;
      u32 k = (f + (s >> 16) % 24) % NKEYS;
      const struct shape *sh = shape_of(k);
      struct mx_tex t;
      u32 t0 = mx_count();
      int r = mx_vcache_tex(k, sh->psm, sh->tw, sh->th, sh->levels, MX_GS_PSM_CT32, &t);
      u32 dt = mx_count() - t0;
      tsum[r == MX_VCACHE_MISS] += dt, nlook[r == MX_VCACHE_MISS]++;
      tmax = dt > tmax ? dt : tmax;
      CHECK(r != MX_VCACHE_FULL);
      if (r == MX_VCACHE_MISS)
        upload(k, &t);
    }
    for (u32 k = 0; k < NKEYS; k++) {
      struct mx_tex t;
      if (mx_vcache_peek(k, &t))
        bad += verify(k, &t), checked++;
    }
  }
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  mx_printf("vcache %s: %u hits %u misses %u evicted (%u of the frame), %u ring frames, "
            "%u ring places, %u pages (%u small), %u entries read back\n",
            (const char *[]){"AUTO", "COST", "RING"}[policy], st.hits, st.misses, st.evicted,
            st.evicted_now, st.ring_frames, st.ring_places, st.pages, st.small_pages, checked);
  mx_printf("vcache: lookup cycles: hits %u, misses %u on average, %u at most\n",
            tsum[0] / (nlook[0] ? nlook[0] : 1), tsum[1] / (nlook[1] ? nlook[1] : 1), tmax);
  CHECK_EQ(bad, 0);
  CHECK(st.evicted > 0);
  CHECK(st.small_pages > 0);
  CHECK(st.pages <= 12);
}

/* ---- (c) drawn from the cache ------------------------------------------- */

/* texel (x, y) of level 0 of key k as drawn (DECAL, CLUT applied) */
static u32 texel(u32 k, u32 x, u32 y) {
  const struct shape *s = shape_of(k);
  u32 w = 1u << s->tw, i = y * w + x;
  u32 c, g = gen[k];
  if (s->psm == MX_GS_PSM_CT32)
    c = i * 4;
  else {
    u32 ix = s->psm == MX_GS_PSM_T8 ? byte_of(k, 0, g, i) : byte_of(k, 0, g, i / 2) >> (i & 1) * 4 & 15;
    /* CSM1: index i of a 256-entry CLUT is at position i with bits 3 and 4 swapped */
    if (s->psm == MX_GS_PSM_T8)
      ix = (ix & 0xE7) | (ix & 8) << 1 | (ix & 16) >> 1;
    c = ix * 4;
    return (u32)byte_of(k, s->levels, g, c) | (u32)byte_of(k, s->levels, g, c + 1) << 8 |
           (u32)byte_of(k, s->levels, g, c + 2) << 16 | (u32)byte_of(k, s->levels, g, c + 3) << 24;
  }
  return (u32)byte_of(k, 0, g, c) | (u32)byte_of(k, 0, g, c + 1) << 8 |
         (u32)byte_of(k, 0, g, c + 2) << 16 | (u32)byte_of(k, 0, g, c + 3) << 24;
}

static void sprite(struct mx_pkt *p, const struct mx_tex *t, u32 x, u32 y) {
  u32 w = 1u << t->tw, h = 1u << t->th;
  mx_pkt_ad_begin(p, 7, 0);
  mx_pkt_ad(p, MX_GS_REG_TEX1_1, mx_gs_tex1(1, 0, 0, 0, 0, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_TEX0_1, mx_tex_tex0(t, 1, 1));
  mx_pkt_ad(p, MX_GS_REG_PRIM, mx_gs_prim(MX_GS_PRIM_SPRITE, 0, 1, 0, 0, 0, 1, 0, 0));
  mx_pkt_ad(p, MX_GS_REG_UV, mx_gs_uv(0, 0));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x), mx_gs_py(y), 0));
  mx_pkt_ad(p, MX_GS_REG_UV, mx_gs_uv(w << 4, h << 4));
  mx_pkt_ad(p, MX_GS_REG_XYZ2, mx_gs_xyz(mx_gs_px(x + w), mx_gs_py(y + h), 0));
}

/* row y of the sprite of key k at (x0, y0): the bad pixels */
static u32 check_row(u32 k, u32 x0, u32 y0, u32 y) {
  u32 w = 1u << shape_of(k)->tw, bad = 0;
  mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, x0, y0 + y, 64, 1);
  for (u32 x = 0; x < w && x < 64; x++)
    if (px[x] != texel(k, x, y)) {
      if (!bad)
        mx_printf("key %u (%u, %u): %08x, expected %08x\n", k, x, y, px[x], texel(k, x, y));
      bad++;
    }
  return bad;
}

static void drawn(void) {
  /* a CT32 (MIPMAP, level 0 two pages), a small CT32, a T8 and a T4 with
     their CLUTs, looked up in a frame of their own (uploaded if missing) */
  static const u32 want[4] = {0, 1, 3, 4};
  u32 keys[4], n = 0;
  struct mx_tex ts[4];
  mx_vcache_frame(41, 40);
  for (u32 i = 0; i < 4; i++) {
    u32 k = want[i];
    const struct shape *sh = shape_of(k);
    int r = mx_vcache_tex(k, sh->psm, sh->tw, sh->th, sh->levels, MX_GS_PSM_CT32, &ts[n]);
    CHECK(r != MX_VCACHE_FULL);
    if (r == MX_VCACHE_MISS)
      upload(k, &ts[n]);
    keys[n++] = k;
  }
  /* the places of all four after the lookups (a later miss may move an earlier one in STREAM) */
  for (u32 i = 0; i < n; i++)
    CHECK(mx_vcache_peek(keys[i], &ts[i]));
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 0, 0, 0);
  for (u32 i = 0; i < n; i++)
    sprite(&p, &ts[i], 64 * i, 100);
  mx_gs_finish_ad(&p);
  send(&p);
  u32 bad = 0;
  for (u32 i = 0; i < n; i++) {
    mx_printf("drawn: key %u (%s) at %u, CLUT %u\n", keys[i],
              (const char *[]){"CT32", "CT32", "", "T8", "T4", ""}[keys[i] % 6], ts[i].tbp[0],
              ts[i].cbp);
    bad += check_row(keys[i], 64 * i, 100, 5) + check_row(keys[i], 64 * i, 100, 30);
  }
  CHECK_EQ(bad, 0);
}

/* ---- (d) a texture drawn, then overwritten in the same stream ------------- */

static void same_stream(void) {
  /* one page: 64 x 32 CT32 textures replace each other */
  mx_vcache_init(1, MX_VCACHE_STREAM, MX_VCACHE_AUTO);
  mx_vcache_frame(100, 99);
  static u8 a[8192] MX_ALIGNED(64), b[8192] MX_ALIGNED(64);
  /* keys 1 and 7 have shape 1 (32 x 32): use 64 x 32 by hand */
  struct mx_tex ta, tb;
  CHECK_EQ(mx_vcache_tex(1001, MX_GS_PSM_CT32, 6, 5, 1, 0, &ta), MX_VCACHE_MISS);
  CHECK_EQ(mx_vcache_tex(1002, MX_GS_PSM_CT32, 6, 5, 1, 0, &tb), MX_VCACHE_MISS);
  CHECK_EQ(ta.tbp[0], tb.tbp[0]); /* the same page */
  CHECK_EQ(mx_vcache_peek(1001, &ta), 0);
  for (u32 i = 0; i < 8192; i++)
    a[i] = (u8)(i * 7 + 1), b[i] = (u8)(i * 13 + 5);
  mx_dcache_writeback(a, sizeof a);
  mx_dcache_writeback(b, sizeof b);
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_draw_env(&p, 0);
  mx_gs_clear(&p, 0, 0, 0);
  mx_gs_upload(&p, tb.tbp[0], tb.tbw[0], MX_GS_PSM_CT32, 0, 0, 64, 32, a);
  sprite(&p, &tb, 0, 40);
  mx_gs_upload(&p, tb.tbp[0], tb.tbw[0], MX_GS_PSM_CT32, 0, 0, 64, 32, b);
  sprite(&p, &tb, 64, 40);
  mx_gs_finish_ad(&p);
  send(&p);
  u32 bad = 0;
  for (u32 s = 0; s < 2; s++) {
    const u32 *src = (const u32 *)(s ? b : a);
    for (u32 y = 0; y < 32; y += 7) {
      mx_gs_download(px, MX_GS_PAGE(mx_gs.fbp[0]), mx_gs.fbw, MX_GS_PSM_CT32, 64 * s, 40 + y, 64, 1);
      for (u32 x = 0; x < 64; x++)
        if (px[x] != src[y * 64 + x]) {
          if (!bad)
            mx_printf("same stream: sprite %u (%u, %u) %08x, expected %08x\n", s, x, y, px[x],
                      src[y * 64 + x]);
          bad++;
        }
    }
  }
  CHECK_EQ(bad, 0);
}


/* ---- (a) the pages ---------------------------------------------------------- */

static void pages(void) {
  /* 640 x 224 CT32: 10 x 7 pages a buffer, from page 0 as before */
  CHECK_EQ(mx_gs.fbp[0], 0);
  CHECK_EQ(mx_gs.fbp[1], 70);
  CHECK_EQ(mx_gs.zbp, 140);
  CHECK_EQ(mx_gs.vram, 210);
  CHECK_EQ(mx_gs.tbp, MX_GS_PAGE(210));
  CHECK_EQ(mx_vram_count(MX_VRAM_FIXED), 210);
  u32 a = mx_vram_alloc(4), b = mx_vram_alloc(2);
  CHECK_EQ(a, 210);
  CHECK_EQ(b, 214);
  CHECK_EQ(mx_gs.tbp, MX_GS_PAGE(216));
  mx_vram_free(a, 4);
  CHECK_EQ(mx_gs.tbp, MX_GS_PAGE(216));
  CHECK_EQ(mx_vram_alloc(3), 210); /* into the hole */
  mx_vram_free(210, 3);
  mx_vram_free(b, 2);
  CHECK_EQ(mx_gs.tbp, MX_GS_PAGE(210));
  CHECK_EQ(mx_vram_alloc(400), ~0u);
  CHECK_EQ(mx_vram_count(MX_VRAM_FIXED), 210);
}

#define STEP(x)                                                                            \
  do {                                                                                     \
    mx_printf("- %s\n", #x);                                                               \
    x;                                                                                     \
  } while (0)

int main(void) {
  mx_gs_init(640, 224);
  STEP(pages());
  /* the identity CLUT (8 x 2 CT32) in a page of the test's own */
  static u32 ids[16] MX_ALIGNED(64);
  for (u32 i = 0; i < 16; i++)
    ids[i] = 0x80000000u | i;
  mx_dcache_writeback(ids, sizeof ids);
  idclut = MX_GS_PAGE(mx_vram_alloc(1));
  struct mx_pkt p;
  mx_pkt_begin(&p, pk, sizeof pk, 0);
  mx_pkt_cnt(&p, 0, 0);
  mx_gs_upload(&p, idclut, 1, MX_GS_PSM_CT32, 0, 0, 8, 2, ids);
  mx_gs_finish_ad(&p);
  send(&p);
  STEP(cache_load(MX_VCACHE_COST));
  STEP(cache_load(MX_VCACHE_RING));
  STEP(cache_load(MX_VCACHE_AUTO));
  STEP(drawn());
  STEP(same_stream());
  mx_vcache_reset();
  CHECK_EQ(mx_vram_count(MX_VRAM_CACHE), 0);
  return t_done("vcache");
}
