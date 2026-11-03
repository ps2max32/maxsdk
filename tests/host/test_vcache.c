/* GS memory by pages (<mx/vram.h>) and the cache in it (<mx/vcache.h>):
   places, eviction by mode, pins, the limits, and a working set too large
   for the cache. After every step the places of all entries are checked
   from the outside (mx_vcache_peek): no two overlap, each lies in pages of
   the cache, small ones within a page from a multiple of their power of
   two, large ones on whole pages. */

#include <mx/tex.h>
#include <mx/vcache.h>
#include <mx/vram.h>
#include "test.h"

#define CHECK(c) CHECK_EQ(!!(c), 1)

static void pages(void) {
  mx_vram_clear();
  CHECK_EQ(mx_vram_find(10), 0);
  CHECK_EQ(mx_vram_take(0, 10, MX_VRAM_FIXED), 0);
  CHECK_EQ(mx_vram_find(5), 10);
  CHECK_EQ(mx_vram_take(8, 4, MX_VRAM_APP), -1); /* 8, 9 are taken */
  CHECK_EQ(mx_vram_owner(10), MX_VRAM_FREE);
  CHECK_EQ(mx_vram_take(510, 3, MX_VRAM_APP), -1); /* past the end */
  CHECK_EQ(mx_vram_take(10, 1, MX_VRAM_FREE), -1);
  mx_vram_give(2, 3);
  CHECK_EQ(mx_vram_find(3), 2);
  CHECK_EQ(mx_vram_find(4), 10);
  CHECK_EQ(mx_vram_count(MX_VRAM_FIXED), 7);
  CHECK_EQ(mx_vram_end(MX_VRAM_FIXED), 10);
  CHECK_EQ(mx_vram_end(MX_VRAM_CACHE), 0);
  CHECK_EQ(mx_vram_find(0), ~0u);
  CHECK_EQ(mx_vram_find(513), ~0u);
  CHECK_EQ(mx_vram_find(503), ~0u);
  CHECK_EQ(mx_vram_find(502), 10);
  mx_vram_clear();
  CHECK_EQ(mx_vram_find(512), 0);
}

/* ---- the cache ------------------------------------------------------------- */

#define NKEYS 4096
struct shape {
  u32 psm, tw, th, levels, cpsm;
};
static u8 known[NKEYS];
static struct shape shapes[NKEYS];

static u32 piece(u32 w, u32 h, u32 psm) {
  u32 b = mx_tex_blocks(w, h, psm);
  return b >= 32 ? (b + 31) & ~31u : b;
}

static int indexed(u32 psm) { return psm == MX_GS_PSM_T8 || psm == MX_GS_PSM_T4; }

/* the places of every entry, checked */
/* the top bytes given to the cache (hbyte()): blocks h_lo .. h_hi - 1 */
static u32 h_lo, h_hi;

static void check_places(u32 max_pages) {
  static u16 owner[MX_VRAM_BLOCKS], howner[MX_VRAM_BLOCKS];
  for (u32 i = 0; i < MX_VRAM_BLOCKS; i++)
    owner[i] = howner[i] = 0;
  int bad = 0;
  u32 used = 0, hused = 0;
  for (u32 k = 0; k < NKEYS; k++) {
    struct mx_tex t;
    if (!known[k] || !mx_vcache_peek(k, &t))
      continue;
    const struct shape *s = &shapes[k];
    u32 bp[MX_TEX_LEVELS + 1], nb[MX_TEX_LEVELS + 1], n = 0;
    if (t.psm == MX_GS_PSM_T8H) {
      /* the levels in the top bytes, laid out as mx_tex_init from the first */
      struct mx_tex lay;
      u32 len = mx_tex_init(&lay, t.tbp[0], MX_GS_PSM_T8H, s->tw, s->th, s->levels, MX_TEX_PACKED);
      if (s->psm != MX_GS_PSM_T8 || t.tbp[0] < h_lo || t.tbp[0] + len > h_hi) {
        printf("key %u: T8H at %u + %u outside the top bytes\n", k, t.tbp[0], len);
        bad++;
      }
      for (u32 l = 0; l < s->levels; l++)
        if (t.tbp[l] != lay.tbp[l] || t.tbw[l] != lay.tbw[l])
          bad++;
      for (u32 b = t.tbp[0]; b < t.tbp[0] + len && b < MX_VRAM_BLOCKS; b++) {
        if (howner[b]) {
          printf("key %u: top byte block %u also key %u\n", k, b, howner[b] - 1u);
          bad++;
          break;
        }
        howner[b] = (u16)(k + 1);
      }
      hused += len;
      n = s->levels;
      goto clut;
    }
    for (u32 l = 0; l < s->levels; l++, n++) {
      bp[n] = t.tbp[l];
      nb[n] = piece(1u << (s->tw > l ? s->tw - l : 0), 1u << (s->th > l ? s->th - l : 0), s->psm);
      if (t.tbw[l] != mx_tex_tbw(1u << (s->tw > l ? s->tw - l : 0), s->psm))
        bad++;
    }
  clut:
    if (indexed(s->psm) && s->cpsm != MX_VCACHE_NO_CLUT) {
      bp[n] = t.cbp;
      nb[n++] = piece(s->psm == MX_GS_PSM_T4 ? 8 : 16, s->psm == MX_GS_PSM_T4 ? 2 : 16, s->cpsm);
    }
    for (u32 i = t.psm == MX_GS_PSM_T8H ? s->levels : 0; i < n; i++) {
      u32 a = 1;
      while (a < nb[i] && a < 32)
        a <<= 1;
      if (bp[i] % a || (nb[i] < 32 && (bp[i] % 32) + nb[i] > 32) || bp[i] + nb[i] > MX_VRAM_BLOCKS) {
        printf("key %u piece %u: %u + %u misplaced\n", k, i, bp[i], nb[i]);
        bad++;
        continue;
      }
      for (u32 b = bp[i]; b < bp[i] + nb[i]; b++) {
        if (owner[b]) {
          printf("key %u piece %u: block %u also key %u\n", k, i, b, owner[b] - 1u);
          bad++;
          break;
        }
        owner[b] = (u16)(k + 1);
        if (mx_vram_owner(b / 32) != MX_VRAM_CACHE) {
          printf("key %u: block %u in a page not the cache's\n", k, b);
          bad++;
          break;
        }
      }
      used += nb[i];
    }
  }
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(bad, 0);
  CHECK_EQ(st.used_blocks, used);
  int mx__vcache_check(void);
  CHECK_EQ(mx__vcache_check(), 0);
  CHECK_EQ(st.hbyte_used, hused);
  CHECK_EQ(st.pages, mx_vram_count(MX_VRAM_CACHE));
  if (max_pages)
    CHECK(st.pages <= max_pages);
  /* a page of the cache holds something */
  u32 empty = 0;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++)
    if (mx_vram_owner(p) == MX_VRAM_CACHE) {
      u32 any = 0;
      for (u32 b = p * 32; b < p * 32 + 32; b++)
        any |= owner[b];
      empty += !any;
    }
  CHECK_EQ(empty, 0);
}

static int tex(u32 k, u32 psm, u32 tw, u32 th, u32 levels, struct mx_tex *t) {
  known[k] = 1;
  shapes[k] = (struct shape){psm, tw, th, levels, MX_GS_PSM_CT32};
  return mx_vcache_tex(k, psm, tw, th, levels, MX_GS_PSM_CT32, t);
}

static void setup_p(u32 fixed, u32 max_pages, int mode, int policy) {
  for (u32 k = 0; k < NKEYS; k++)
    known[k] = 0;
  mx_vram_clear();
  if (fixed)
    mx_vram_take(0, fixed, MX_VRAM_FIXED);
  mx_vcache_init(max_pages, mode, policy);
  mx_vcache_frame(0, 0);
  h_lo = h_hi = 0;
}
static void setup(u32 fixed, u32 max_pages, int mode) { setup_p(fixed, max_pages, mode, MX_VCACHE_AUTO); }

static void basics(void) {
  struct mx_tex t, u;
  setup(400, 0, MX_VCACHE_STREAM);
  /* 64 x 64 CT32, 4 levels: level 0 two pages, the others small */
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 6, 6, 4, &t), MX_VCACHE_MISS);
  CHECK_EQ(t.tbp[0], 400 * 32);
  CHECK_EQ(t.tbw[0], 1);
  CHECK_EQ(t.blocks, 64 + 16 + 4 + 1);
  CHECK_EQ(t.tbp[1], 402 * 32);     /* 16 blocks */
  CHECK_EQ(t.tbp[2], 402 * 32 + 16); /* 4 */
  CHECK_EQ(t.tbp[3], 402 * 32 + 20); /* 1 */
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 6, 6, 4, &u), MX_VCACHE_HIT);
  CHECK_EQ(u.tbp[0], t.tbp[0]);
  CHECK_EQ(u.tbp[3], t.tbp[3]);
  /* T8 with its CLUT (4 blocks) and T4 with its (1): into the small page */
  CHECK_EQ(tex(2, MX_GS_PSM_T8, 4, 4, 1, &t), MX_VCACHE_MISS); /* 16 x 16 T8: 1 block */
  CHECK_EQ(t.cpsm, MX_GS_PSM_CT32);
  CHECK(t.cbp != 0);
  CHECK_EQ(tex(3, MX_GS_PSM_T4, 5, 4, 1, &t), MX_VCACHE_MISS);
  check_places(0);
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(st.pages, 3);
  CHECK_EQ(st.small_pages, 1);
  CHECK_EQ(st.hits, 1);
  CHECK_EQ(st.misses, 3);
  /* a shared CLUT as a buffer */
  u32 bp;
  CHECK_EQ(mx_vcache_buf(100, 16, 16, MX_GS_PSM_CT32, &bp), MX_VCACHE_MISS);
  CHECK_EQ(bp % 4, 0);
  CHECK_EQ(mx_vcache_buf(100, 16, 16, MX_GS_PSM_CT32, &bp), MX_VCACHE_HIT);
  CHECK_EQ(mx_vcache_drop(100), 0); /* (check_places knows textures only) */
  /* another shape for key 1: the old one goes */
  CHECK_EQ(tex(1, MX_GS_PSM_CT16, 5, 5, 1, &t), MX_VCACHE_MISS);
  check_places(0);
  /* drop: its pages go back */
  CHECK_EQ(mx_vcache_drop(1), 0);
  CHECK_EQ(mx_vcache_drop(1), -1);
  CHECK_EQ(mx_vcache_peek(1, &t), 0);
  known[1] = 0;
  check_places(0);
  mx_vcache_reset();
  CHECK_EQ(mx_vram_count(MX_VRAM_CACHE), 0);
  CHECK_EQ(mx_vram_count(MX_VRAM_FIXED), 400);
}

/* a level larger than the cache may get: MX_VCACHE_FULL at once */
static void too_large(void) {
  struct mx_tex t;
  setup(0, 4, MX_VCACHE_STREAM);
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 8, 8, 1, &t), MX_VCACHE_FULL); /* 64 pages */
  CHECK_EQ(mx_vram_count(MX_VRAM_CACHE), 0);
  /* whole GS memory taken by others */
  setup(512, 0, MX_VCACHE_STREAM);
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 3, 3, 1, &t), MX_VCACHE_FULL);
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(st.full, 1);
  CHECK_EQ(st.entries, 0);
}

/* random shapes and keys over many frames, within 12 pages */
static u32 rnd_s = 12345;
static u32 rnd(void) { return rnd_s = rnd_s * 1103515245u + 12345u, rnd_s >> 16; }

static void random_load(int mode, int policy) {
  static const u32 psms[4] = {MX_GS_PSM_CT32, MX_GS_PSM_CT16, MX_GS_PSM_T8, MX_GS_PSM_T4};
  struct mx_tex t;
  setup_p(100, 12, mode, policy);
  rnd_s = 12345;
  /* each key keeps its shape */
  static struct shape fixed_shape[200];
  for (u32 k = 0; k < 200; k++) {
    u32 psm = psms[rnd() % 4], tw = 3 + rnd() % 4, th = 3 + rnd() % 4;
    if (psm == MX_GS_PSM_T8 || psm == MX_GS_PSM_T4)
      tw = tw < 4 ? 4 : tw, th = th < 4 ? 4 : th;
    fixed_shape[k] = (struct shape){psm, tw, th, 1 + rnd() % 3, MX_GS_PSM_CT32};
  }
  u32 bad_frame = 0;
  for (u32 f = 1; f < 300; f++) {
    mx_vcache_frame(f, f - 2);
    u32 used_now[24], n_now = 0;
    for (u32 i = 0; i < 24; i++) {
      u32 k = (f + rnd() % 40) % 200;
      const struct shape *s = &fixed_shape[k];
      int r = tex(k, s->psm, s->tw, s->th, s->levels, &t);
      if (r != MX_VCACHE_FULL)
        used_now[n_now++] = k;
    }
    check_places(12);
    /* REPLAY and ASYNC keep what this frame used */
    if (mode != MX_VCACHE_STREAM)
      for (u32 i = 0; i < n_now; i++)
        bad_frame += !mx_vcache_peek(used_now[i], &t);
  }
  CHECK_EQ(bad_frame, 0);
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK(st.hits > 0);
  CHECK(st.evicted > 0);
  if (mode != MX_VCACHE_STREAM)
    CHECK_EQ(st.evicted_now, 0);
  printf("vcache random (mode %d, policy %d): %u hits %u misses %u full %u evicted (%u done), "
         "%u pages %u small\n",
         mode, policy, st.hits, st.misses, st.full, st.evicted, st.evicted_done, st.pages,
         st.small_pages);
  if (policy == MX_VCACHE_RING)
    CHECK(st.ring_places > 0);
  if (policy != MX_VCACHE_RING)
    CHECK_EQ(st.ring_places, 0);
}

/* ASYNC: an entry of a frame the GS is not done with stays */
static void async_fence(void) {
  struct mx_tex t;
  setup(0, 4, MX_VCACHE_ASYNC);
  mx_vcache_frame(1, 0);
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 7, 6, 1, &t), MX_VCACHE_MISS); /* 128 x 64: 4 pages */
  mx_vcache_frame(2, 1);                                           /* frame 1 not done */
  CHECK_EQ(tex(2, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_FULL);
  CHECK_EQ(mx_vcache_peek(1, &t), 1);
  CHECK_EQ(mx_vcache_drop(1), -1);
  mx_vcache_frame(3, 2); /* now it is */
  CHECK_EQ(tex(2, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS);
  CHECK_EQ(mx_vcache_peek(1, &t), 0);
  check_places(4);
}

/* pins hold; STREAM may evict what this frame used */
static void pins(void) {
  struct mx_tex t;
  setup(0, 2, MX_VCACHE_STREAM);
  CHECK_EQ(tex(1, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS); /* 1 page */
  CHECK_EQ(mx_vcache_pin(1), 0);
  CHECK_EQ(tex(2, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS);
  CHECK_EQ(tex(3, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS); /* 2 goes */
  CHECK_EQ(mx_vcache_peek(1, &t), 1);
  CHECK_EQ(mx_vcache_peek(2, &t), 0);
  CHECK_EQ(tex(4, MX_GS_PSM_CT32, 7, 5, 1, &t), MX_VCACHE_FULL); /* 2 pages: 1 is pinned */
  mx_vcache_unpin(1);
  CHECK_EQ(tex(4, MX_GS_PSM_CT32, 7, 5, 1, &t), MX_VCACHE_MISS);
  check_places(2);
  CHECK_EQ(mx_vcache_pin(99), -1);
}

/* 20 one-page textures each frame, in the same order, through 16 pages:
   16 stay: the 4 placed last evict those done for the frame (a ring or
   LRU would miss all 20 every frame) */
static u32 too_many(int policy) {
  struct mx_tex t;
  setup_p(0, 16, MX_VCACHE_STREAM, policy);
  u32 hits_last = 0;
  for (u32 f = 1; f <= 10; f++) {
    mx_vcache_frame(f, f - 1);
    hits_last = 0;
    for (u32 k = 0; k < 20; k++)
      hits_last += tex(k, MX_GS_PSM_CT32, 6, 5, 1, &t) == MX_VCACHE_HIT;
    check_places(16);
  }
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  printf("vcache too many (policy %d): %u hits in the last frame\n", policy, hits_last);
  if (policy != MX_VCACHE_RING)
    CHECK_EQ(st.ring_places, 0);
  return hits_last;
}

/* A frame of a game (STREAM) whose textures are 125% of the cache: 16
   pages; 12 one-page textures drawn once, 4 drawn twice (early and at the
   end), 2 two-page ones; the same frame over and over. Blocks uploaded a
   frame once it repeats, and the evictions of textures done for the
   frame. */
static u32 cyclic(int policy, u32 *done) {
  static const u8 order[] = {12, 13, 0, 1, 2, 16, 3, 4, 5, 14, 6, 7, 8, 17, 9, 10, 11, 15,
                             12, 13, 14, 15};
  struct mx_tex t;
  setup_p(0, 16, MX_VCACHE_STREAM, policy);
  struct mx_vcache_stats st0, st;
  for (u32 f = 1; f <= 20; f++) {
    if (f == 11)
      mx_vcache_stats(&st0);
    mx_vcache_frame(f, f - 1);
    for (u32 i = 0; i < sizeof order; i++) {
      u32 k = order[i];
      int r = k >= 16 ? tex(k, MX_GS_PSM_CT32, 7, 5, 1, &t) : tex(k, MX_GS_PSM_CT32, 6, 5, 1, &t);
      CHECK(r != MX_VCACHE_FULL);
    }
    check_places(16);
  }
  mx_vcache_stats(&st);
  u32 up = (st.upload_blocks - st0.upload_blocks) / 10;
  *done = st.evicted_done - st0.evicted_done;
  printf("vcache cyclic 125%% (policy %d): %u blocks uploaded a frame (%u pages of textures), "
         "%u evictions of done textures\n",
         policy, up, 20, *done);
  return up;
}

/* mx_vcache_use counts as a use: a texture looked up once and used again
   without a lookup is not done for the frame before its last use */
static void use_counts(void) {
  struct mx_tex t;
  setup_p(0, 2, MX_VCACHE_STREAM, MX_VCACHE_COST);
  CHECK_EQ(mx_vcache_use(1), -1);
  for (u32 f = 1; f <= 3; f++) {
    mx_vcache_frame(f, f - 1);
    /* 1 used twice (the second without a lookup), 2 once */
    CHECK(tex(1, MX_GS_PSM_CT32, 6, 5, 1, &t) != MX_VCACHE_FULL);
    CHECK(tex(2, MX_GS_PSM_CT32, 6, 5, 1, &t) != MX_VCACHE_FULL);
    if (f < 3)
      CHECK_EQ(mx_vcache_use(1), 0);
  }
  /* frame 3: 1 used once of twice, 2 done (and used last): 2 goes, not 1 */
  CHECK_EQ(tex(3, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS);
  CHECK_EQ(mx_vcache_peek(1, &t), 1);
  CHECK_EQ(mx_vcache_peek(2, &t), 0);
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(st.evicted_done, 1);
  /* placed again in the next frame, it keeps its counts: done after one use */
  mx_vcache_frame(4, 3);
  CHECK(tex(2, MX_GS_PSM_CT32, 6, 5, 1, &t) != MX_VCACHE_FULL); /* evicts 1 or 3 */
  CHECK(tex(4, MX_GS_PSM_CT32, 6, 5, 1, &t) != MX_VCACHE_FULL);
  mx_vcache_stats(&st);
  CHECK_EQ(st.evicted_done, 2); /* 2 again: done */
  check_places(2);
}

/* ring order: one-page textures take the pages in turn, round and round;
   a pinned one is passed */
static void ring_order(void) {
  struct mx_tex t;
  u32 at[8];
  setup_p(0, 4, MX_VCACHE_STREAM, MX_VCACHE_RING);
  for (u32 k = 0; k < 4; k++) {
    CHECK_EQ(tex(k, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS);
    at[k] = t.tbp[0];
  }
  CHECK_EQ(mx_vcache_pin(1), 0);
  for (u32 k = 4; k < 8; k++) {
    CHECK_EQ(tex(k, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS);
    at[k] = t.tbp[0];
  }
  CHECK_EQ(at[4], at[0]);
  CHECK_EQ(at[5], at[2]); /* page of 1 passed */
  CHECK_EQ(at[6], at[3]);
  CHECK_EQ(at[7], at[0]);
  CHECK_EQ(mx_vcache_peek(1, &t), 1);
  check_places(4);
  /* small pieces fill the ring's page one after the other */
  setup_p(0, 2, MX_VCACHE_STREAM, MX_VCACHE_RING);
  CHECK_EQ(tex(10, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS); /* page 0 */
  CHECK_EQ(tex(11, MX_GS_PSM_CT32, 6, 5, 1, &t), MX_VCACHE_MISS); /* page 1 */
  CHECK_EQ(tex(12, MX_GS_PSM_CT32, 5, 5, 1, &t), MX_VCACHE_MISS); /* 16 blocks: page 0 goes */
  CHECK_EQ(t.tbp[0], 0);
  CHECK_EQ(tex(13, MX_GS_PSM_CT32, 4, 4, 1, &t), MX_VCACHE_MISS); /* free blocks: 16 */
  CHECK_EQ(t.tbp[0], 16);
  CHECK_EQ(tex(14, MX_GS_PSM_CT32, 5, 5, 1, &t), MX_VCACHE_MISS); /* after 12 in ring order: 13 goes */
  CHECK_EQ(t.tbp[0], 16);
  CHECK_EQ(mx_vcache_peek(13, &t), 0);
  CHECK_EQ(mx_vcache_peek(11, &t), 1);
  CHECK_EQ(tex(15, MX_GS_PSM_CT32, 5, 5, 1, &t), MX_VCACHE_MISS); /* page 0 full: page 1 goes */
  CHECK_EQ(t.tbp[0], 32);
  CHECK_EQ(mx_vcache_peek(11, &t), 0);
  check_places(2);
}

/* a large level among small pieces: the cheapest run of pages goes */
static void fragmented(void) {
  struct mx_tex t;
  setup(0, 8, MX_VCACHE_STREAM);
  /* 8 pages of 16-block pieces, frame 1 */
  mx_vcache_frame(1, 0);
  u32 k = 0;
  for (; k < 16; k++)
    CHECK_EQ(tex(k, MX_GS_PSM_CT32, 5, 5, 1, &t), MX_VCACHE_MISS);
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(st.pages, 8);
  /* frame 9: those of pages 2 and 3 used again (keys 4..7) */
  mx_vcache_frame(9, 8);
  for (u32 i = 4; i < 8; i++)
    CHECK_EQ(tex(i, MX_GS_PSM_CT32, 5, 5, 1, &t), MX_VCACHE_HIT);
  mx_vcache_frame(10, 9);
  /* a 2-page level: not pages 2 and 3 */
  CHECK_EQ(tex(100, MX_GS_PSM_CT32, 7, 5, 1, &t), MX_VCACHE_MISS);
  CHECK(t.tbp[0] / 32 + 2 <= 2 || t.tbp[0] / 32 >= 4);
  for (u32 i = 4; i < 8; i++)
    CHECK_EQ(mx_vcache_peek(i, &t), 1);
  check_places(8);
}

/* more keys than entries: the table and the hash keep up */
static void many_keys(void) {
  struct mx_tex t;
  setup(0, 0, MX_VCACHE_STREAM);
  for (u32 k = 0; k < 3000; k++) {
    mx_vcache_frame(k / 100, k / 100);
    CHECK(tex(k, MX_GS_PSM_CT32, 3, 3, 1, &t) == MX_VCACHE_MISS);
  }
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  CHECK_EQ(st.entries, MX_VCACHE_ENTRIES);
  CHECK_EQ(st.evicted, 3000 - MX_VCACHE_ENTRIES);
  /* the newest are there, and hit */
  for (u32 k = 2990; k < 3000; k++)
    CHECK_EQ(tex(k, MX_GS_PSM_CT32, 3, 3, 1, &t), MX_VCACHE_HIT);
  check_places(0);
}

/* an indexed texture without a CLUT of its own (MX_VCACHE_NO_CLUT): no
   CLUT piece, and not the same shape as the same texture with one */
static void no_clut(void) {
  struct mx_tex t;
  setup(400, 0, MX_VCACHE_STREAM);
  /* for check_places */
  known[1] = 1;
  shapes[1] = (struct shape){.psm = MX_GS_PSM_T8, .tw = 4, .th = 4, .levels = 1, .cpsm = MX_VCACHE_NO_CLUT};
  CHECK_EQ(mx_vcache_tex(1, MX_GS_PSM_T8, 4, 4, 1, MX_VCACHE_NO_CLUT, &t), MX_VCACHE_MISS);
  CHECK_EQ(t.blocks, 1); /* 16 x 16 T8: the level alone */
  CHECK_EQ(t.cbp, 0);
  CHECK_EQ(mx_vcache_tex(1, MX_GS_PSM_T8, 4, 4, 1, MX_VCACHE_NO_CLUT, &t), MX_VCACHE_HIT);
  CHECK_EQ(mx_vcache_tex(1, MX_GS_PSM_T8, 4, 4, 1, MX_GS_PSM_CT32, &t), MX_VCACHE_MISS);
  shapes[1].cpsm = MX_GS_PSM_CT32;
  CHECK_EQ(t.blocks, 1 + 4);
  CHECK(t.cbp != 0);
  check_places(0);
}

/* T8 textures in the top bytes of a 24-bit buffer (pages 0 .. 39, the
   application's): placed there as T8H while that room is free, CLUTs in
   pages of the cache; then in pages of the cache; over frames the top
   bytes are reused (evicted by cost like the rest); other formats never
   go there */
static void hbyte(void) {
  struct mx_tex t;
  setup_p(40, 0, MX_VCACHE_STREAM, MX_VCACHE_AUTO);
  mx_vcache_hpages(0, 40);
  h_lo = 0, h_hi = 40 * 32;
  u32 nh = 0, nn = 0;
  /* 64 x 64 T8, 4 levels: 2 pages of top bytes + 16 + 4 + 1 blocks */
  for (u32 k = 0; k < 24; k++) {
    CHECK_EQ(tex(k, MX_GS_PSM_T8, 6, 6, 4, &t), MX_VCACHE_MISS);
    nh += t.psm == MX_GS_PSM_T8H, nn += t.psm == MX_GS_PSM_T8;
    check_places(0);
  }
  struct mx_vcache_stats st;
  mx_vcache_stats(&st);
  printf("vcache top bytes: %u of 24 textures there (%u of %u blocks), %u in pages\n", nh,
         st.hbyte_used, st.hbyte_blocks, nn);
  CHECK_EQ(nh, 13); /* 85 blocks each, from a page: 3 pages */
  CHECK_EQ(nh + nn, 24);
  CHECK_EQ(tex(30, MX_GS_PSM_CT16, 6, 6, 1, &t), MX_VCACHE_MISS);
  CHECK_EQ(t.psm, MX_GS_PSM_CT16);
  /* a hit keeps its place and psm */
  CHECK_EQ(tex(0, MX_GS_PSM_T8, 6, 6, 4, &t), MX_VCACHE_HIT);
  CHECK_EQ(t.psm, MX_GS_PSM_T8H);
  /* the pages of the cache limited to 6 (a texture and its CLUT, 26
     blocks: one a page): 20 textures a frame, room for 19: the cache
     keeps all but one or two from frame to frame, the top bytes reused */
  setup_p(40, 6, MX_VCACHE_STREAM, MX_VCACHE_AUTO);
  mx_vcache_hpages(0, 40);
  h_lo = 0, h_hi = 40 * 32;
  u32 hits = 0;
  for (u32 f = 1; f <= 8; f++) {
    mx_vcache_frame(f, f - 1);
    hits = 0;
    for (u32 k = 0; k < 20; k++) {
      int r = tex(100 + k, MX_GS_PSM_T8, 6, 6, 4, &t);
      CHECK(r != MX_VCACHE_FULL);
      hits += r == MX_VCACHE_HIT;
    }
    check_places(6);
  }
  mx_vcache_stats(&st);
  printf("vcache top bytes over frames: %u places there, %u evicted, %u hits in the last frame\n",
         st.hbyte_places, st.evicted, hits);
  CHECK(st.hbyte_places >= 13);
  CHECK(hits >= 17);
  mx_vcache_hpages(0, 0);
  check_places(6);
}

void test_vcache(void) {
  pages();
  basics();
  no_clut();
  too_large();
  for (int pol = MX_VCACHE_AUTO; pol <= MX_VCACHE_RING; pol++) {
    random_load(MX_VCACHE_STREAM, pol);
    random_load(MX_VCACHE_REPLAY, pol);
    random_load(MX_VCACHE_ASYNC, pol);
  }
  async_fence();
  pins();
  CHECK_EQ(too_many(MX_VCACHE_COST), 16); /* the 4 done for the frame make room */
  CHECK_EQ(too_many(MX_VCACHE_AUTO), 16);
  too_many(MX_VCACHE_RING);
  u32 d_ring, d_cost;
  u32 up_ring = cyclic(MX_VCACHE_RING, &d_ring), up_cost = cyclic(MX_VCACHE_AUTO, &d_cost);
  CHECK(up_cost * 2 < up_ring);
  CHECK(up_cost < 128 + 32); /* 4 pages at least */
  CHECK(d_cost > 0);
  use_counts();
  hbyte();
  ring_order();
  fragmented();
  many_keys();
}
