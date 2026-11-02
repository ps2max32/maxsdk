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
static void check_places(u32 max_pages) {
  static u16 owner[MX_VRAM_BLOCKS];
  for (u32 i = 0; i < MX_VRAM_BLOCKS; i++)
    owner[i] = 0;
  int bad = 0;
  u32 used = 0;
  for (u32 k = 0; k < NKEYS; k++) {
    struct mx_tex t;
    if (!known[k] || !mx_vcache_peek(k, &t))
      continue;
    const struct shape *s = &shapes[k];
    u32 bp[MX_TEX_LEVELS + 1], nb[MX_TEX_LEVELS + 1], n = 0;
    for (u32 l = 0; l < s->levels; l++, n++) {
      bp[n] = t.tbp[l];
      nb[n] = piece(1u << (s->tw > l ? s->tw - l : 0), 1u << (s->th > l ? s->th - l : 0), s->psm);
      if (t.tbw[l] != mx_tex_tbw(1u << (s->tw > l ? s->tw - l : 0), s->psm))
        bad++;
    }
    if (indexed(s->psm)) {
      bp[n] = t.cbp;
      nb[n++] = piece(s->psm == MX_GS_PSM_T4 ? 8 : 16, s->psm == MX_GS_PSM_T4 ? 2 : 16, s->cpsm);
    }
    for (u32 i = 0; i < n; i++) {
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
  printf("vcache random (mode %d, policy %d): %u hits %u misses %u full %u evicted, %u ring frames, "
         "%u pages %u small\n",
         mode, policy, st.hits, st.misses, st.full, st.evicted, st.ring_frames, st.pages,
         st.small_pages);
  if (policy == MX_VCACHE_RING)
    CHECK(st.ring_places > 0);
  if (policy == MX_VCACHE_COST)
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
   the 15 used first stay (a ring or LRU would miss all 20 every frame) */
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
  printf("vcache too many (policy %d): %u hits in the last frame, %u ring frames\n", policy,
         hits_last, st.ring_frames);
  if (policy == MX_VCACHE_AUTO)
    CHECK_EQ(st.ring_frames, 10); /* every frame: the 17th does not fit */
  return hits_last;
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

void test_vcache(void) {
  pages();
  basics();
  too_large();
  for (int pol = MX_VCACHE_AUTO; pol <= MX_VCACHE_RING; pol++) {
    random_load(MX_VCACHE_STREAM, pol);
    random_load(MX_VCACHE_REPLAY, pol);
    random_load(MX_VCACHE_ASYNC, pol);
  }
  async_fence();
  pins();
  CHECK_EQ(too_many(MX_VCACHE_COST), 15);
  too_many(MX_VCACHE_AUTO);
  too_many(MX_VCACHE_RING);
  ring_order();
  fragmented();
  many_keys();
}
