/* GS memory as a cache (<mx/vcache.h>).

   Each GS block (64 words) of a page the cache has is marked with the
   entry it holds (own), each page with the blocks in use (mask) and what
   it holds (pkind: one large piece, or small pieces). Making room weighs
   every entry once (wt: per block, INF if it must stay; ecost: the
   whole entry), then takes the run whose entries cost least in all, each
   entry counted once however many of its blocks lie in the run. */

#include <mx/tex.h>
#include <mx/vcache.h>
#include <mx/vram.h>
#ifdef MX_HOST
#include <stdio.h>
#define mx_printf printf
#else
#include <mx/mx.h>
#endif

const u8 *mx__vram_owners(void); /* vram.c: each page's owner */

#define NE MX_VCACHE_ENTRIES
#define NH (2 * NE) /* hash slots, a power of two */
#define NP (MX_TEX_LEVELS + 1)
#define INF (~0ull)
/* STREAM: the bands of entries the last two frames used, above every
   weight of an older entry (below 1 << 20) */
#define DONE_BASE (1ull << 20) /* this frame's, done for it */
#define PREV_BASE (1ull << 21) /* the last frame's, not yet this frame's */
#define NOW_BASE (1ull << 22)  /* this frame's */
#define NHIST 256              /* history of evicted entries, a power of two */

enum { K_TEX, K_BUF };
enum { PG_NONE, PG_BIG, PG_SMALL };

struct ent {
  u64 key;
  u32 last_use;   /* frame */
  u32 useq;       /* the use counter at its last use */
  u16 uses;       /* uses in its last frame */
  u16 uses_prev;  /* uses in the frame before that (0: not used then) */
  u32 bp[NP];     /* each piece's first block (~0u: not placed) */
  u16 nb[NP];     /* each piece's blocks */
  u16 w, h;       /* K_TEX: tw, th; K_BUF: pixels */
  u16 blocks;     /* all pieces in pages of the cache */
  u16 hn;         /* T8 placed as T8H: its levels' blocks from hb in the top bytes (hpages) */
  u32 hb;
  u8 kind, psm, cpsm, levels, npieces, pins, freq, building, live;
};

static struct ent ents[NE];
static u16 free_ents[NE];
static u32 nfree;
static u32 ent_hi; /* one past the highest entry handed out since the reset */
static u16 htab[NH];                /* entry + 1, 0 empty */
static u16 own[MX_VRAM_BLOCKS];     /* entry + 1, 0 free */
static u32 mask[MX_VRAM_PAGES];     /* blocks in use, pages of the cache */
static u8 pkind[MX_VRAM_PAGES];
static u32 max_pages, npages, nsmall, used_blocks, mode, now, done, seq;
/* seq when this frame and the one before began (mx_vcache_frame) */
static u32 seq0, seq0_prev;
static u32 policy;
/* ring order: the page it goes on from, the page of small pieces it fills
   and the block there; the frame AUTO went to ring order in */
static u32 ring_page, ring_small = ~0u, ring_blk;
static struct mx_vcache_stats st;
/* how many times the cache was set up (mode changes: the frame numbers start again) */
static u32 inits;
/* the entry of each page of a large piece (entry + 1, 0: none) */
static u16 pent[MX_VRAM_PAGES];
/* the top bytes of 24-bit buffers (mx_vcache_hpages): blocks h_first ..
   h_first + h_n - 1, the entry of each (entry + 1, 0: free) */
static u32 h_first, h_n, h_used;
static u16 hown[MX_VRAM_BLOCKS];
/* a page of top bytes whose blocks are all one entry's, or all free */
static u8 h_uni[MX_VRAM_PAGES];

static void h_note(u32 b, u32 n) {
  for (u32 p = b / 32; p <= (b + n - 1) / 32; p++) {
    u32 v = hown[p * 32], i = 1;
    while (i < 32 && hown[p * 32 + i] == v)
      i++;
    h_uni[p] = i == 32;
  }
}
/* while making room: each entry's weight, valid in frame wt_now (an
   entry's own changes update it) */
static u64 wt[NE], ecost[NE];
static u32 wt_now = ~0u;
static u32 cnt[NE], stamp[NE], stamp_n;
/* the uses of entries evicted (direct mapped by key): an entry placed
   again goes on with them */
static struct hist {
  u64 key;
  u32 last_use;
  u16 uses, uses_prev;
  u8 freq, live;
} hist[NHIST];

/* ---- the key -> entry hash (linear probing, deletion by backward shift) --- */

/* 32-bit multiplies only (the R5900 has no 64-bit one: a library call) */
static u32 hash(u64 k) {
  u32 x = ((u32)k ^ (u32)(k >> 32) * 0x85EBCA6Bu) * 0x9E3779B1u;
  return (x ^ x >> 16) & (NH - 1);
}

static int find(u64 key) {
  for (u32 i = hash(key);; i = (i + 1) & (NH - 1)) {
    if (!htab[i])
      return -1;
    if (ents[htab[i] - 1].key == key)
      return htab[i] - 1;
  }
}

static void h_insert(u32 e) {
  u32 i = hash(ents[e].key);
  while (htab[i])
    i = (i + 1) & (NH - 1);
  htab[i] = (u16)(e + 1);
}

static void h_remove(u32 e) {
  u32 i = hash(ents[e].key);
  while (htab[i] != e + 1)
    i = (i + 1) & (NH - 1);
  for (u32 j = i;;) {
    htab[i] = 0;
    for (;;) {
      j = (j + 1) & (NH - 1);
      if (!htab[j])
        return;
      u32 k = hash(ents[htab[j] - 1].key);
      /* the entry at j stays if its home lies cyclically in (i, j] */
      if (i <= j ? (i < k && k <= j) : (i < k || k <= j))
        continue;
      break;
    }
    htab[i] = htab[j];
    i = j;
  }
}

/* ---- pages and blocks ------------------------------------------------------ */

static int page_get(u32 p, u8 kind) {
  if (mx_vram_take(p, 1, MX_VRAM_CACHE) != 0)
    return -1;
  pkind[p] = kind, mask[p] = 0;
  npages++, nsmall += kind == PG_SMALL;
  return 0;
}

static void page_drop(u32 p) {
  mx_vram_give(p, 1);
  npages--, nsmall -= pkind[p] == PG_SMALL;
  pkind[p] = PG_NONE, mask[p] = 0, pent[p] = 0;
}

static void blocks_set(u32 b, u32 n, u32 v) {
  for (u32 i = b; i < b + n; i++) {
    own[i] = (u16)v;
    if (v)
      mask[i >> 5] |= 1u << (i & 31);
    else
      mask[i >> 5] &= ~(1u << (i & 31));
  }
  if (v)
    used_blocks += n;
  else
    used_blocks -= n;
}

/* ---- entries --------------------------------------------------------------- */

static void ent_free(u32 e) {
  struct ent *x = &ents[e];
  u32 i0 = 0;
  if (x->hn) {
    for (u32 b = x->hb; b < x->hb + x->hn; b++)
      hown[b] = 0;
    h_note(x->hb, x->hn);
    h_used -= x->hn;
    x->hn = 0;
    i0 = x->levels; /* the levels are there, the CLUT in a page */
  }
  for (u32 i = i0; i < x->npieces; i++) {
    if (x->bp[i] == ~0u)
      continue;
    u32 b = x->bp[i], n = x->nb[i];
    blocks_set(b, n, 0);
    for (u32 p = b >> 5; p <= (b + n - 1) >> 5; p++)
      if (!mask[p])
        page_drop(p);
  }
  if (!x->building)
    h_remove(e);
  x->live = 0;
  wt[e] = ecost[e] = 0;
  free_ents[nfree++] = (u16)e;
}

static u64 weight(const struct ent *x) {
  if (x->pins || x->building)
    return INF;
  u32 age = now - x->last_use;
  if (mode == MX_VCACHE_ASYNC && (s32)(done - x->last_use) <= 0)
    return INF;
  if (age == 0 && mode != MX_VCACHE_STREAM)
    return INF;
  if (age <= 1 && mode == MX_VCACHE_STREAM) {
    /* the farthest next use goes first: of this frame's entries those done
       for it (as many uses as in the frame before: needed next frame), the
       most recently used first; then the last frame's not used yet (needed
       later in this one), the one used last first; then this frame's
       others, the most recently used first */
    u32 f = x->useq - (age == 1 ? seq0_prev : seq0); /* its last use in that frame */
    u32 d = 0xFFFFF - (f > 0xFFFFF ? 0xFFFFF : f);
    if (age == 1)
      return PREV_BASE + d;
    return (x->uses_prev && x->uses >= x->uses_prev ? DONE_BASE : NOW_BASE) + d;
  }
  u32 sh = age - 1 > 20 ? 20 : age - 1;
  u64 w = ((u64)(16 + x->freq) << 12) >> sh;
  return w ? w : 1;
}

static u32 hslot(u64 key) { return hash(key) & (NHIST - 1); }

static void evict(u32 e) {
  struct ent *x = &ents[e];
  st.evicted++;
  st.evicted_blocks += x->blocks + x->hn;
  if (x->last_use == now) {
    st.evicted_now++;
    st.evicted_done += x->uses_prev && x->uses >= x->uses_prev;
  }
  hist[hslot(x->key)] = (struct hist){.key = x->key, .last_use = x->last_use, .uses = x->uses,
                                      .uses_prev = x->uses_prev, .freq = x->freq, .live = 1};
  ent_free(e);
}

static void reweigh(u32 e) {
  wt[e] = ents[e].live ? weight(&ents[e]) : 0;
  /* what it costs to place again: its bytes (a T8H block holds a quarter of a T8 one's) */
  u32 cb = ents[e].blocks + ents[e].hn / 4u;
  ecost[e] = wt[e] == INF ? INF : wt[e] * (cb ? cb : 1);
}

/* the weights hold within a frame but for the entries' own changes
   (reweigh) */
static void weigh(void) {
  if (wt_now == now)
    return;
  for (u32 e = 0; e < ent_hi; e++)
    reweigh(e);
  wt_now = now;
}

static void evict_range(u32 b, u32 n) {
  /* what the run needed (its blocks in use), against what goes: the
     entries' other blocks too (measured: st.evict_run_blocks) */
  for (u32 i = b; i < b + n; i++)
    st.evict_run_blocks += own[i] != 0;
  for (u32 i = b; i < b + n; i++)
    if (own[i])
      evict(own[i] - 1u);
}

/* The cost of the entries in blocks b .. b + n - 1, each counted once;
   INF if one must stay. */
static u64 range_cost(u32 b, u32 n) {
  u64 c = 0;
  stamp_n++;
  for (u32 i = b; i < b + n; i++) {
    u32 v = own[i];
    if (!v || stamp[v - 1] == stamp_n)
      continue;
    stamp[v - 1] = stamp_n;
    if (ecost[v - 1] == INF)
      return INF;
    c += ecost[v - 1];
  }
  return c;
}

/* The run of n pages that costs least (whole entries; free pages nothing,
   pages of others not at all), within max_pages. Returns its first page
   (*cost) or ~0u. Needs weigh(). One pass, the window sliding on: a page
   of a large piece is read as its entry (pent), a page of small pieces as
   the list of its entries made when it comes in. */
static u32 find_page(u64 *cost);

static u32 find_window(u32 n, u64 *cost) {
  if (n == 1)
    return find_page(cost);
  static u8 bad[MX_VRAM_PAGES], fresh[MX_VRAM_PAGES];
  static u16 plist[MX_VRAM_BLOCKS], pstart[MX_VRAM_PAGES + 1];
  const u8 *owner = mx__vram_owners();
  u64 sum = 0, best = INF;
  u32 at = ~0u, nbad = 0, nfresh = 0, k = 0, p;
  for (p = 0; p < MX_VRAM_PAGES; p++) {
    /* page p comes into the window */
    u32 o = owner[p];
    fresh[p] = o == MX_VRAM_FREE;
    bad[p] = o != MX_VRAM_FREE && o != MX_VRAM_CACHE;
    pstart[p] = (u16)k;
    if (pkind[p] == PG_BIG) {
      u32 v = pent[p];
      plist[k++] = (u16)v;
      if (wt[v - 1] == INF)
        bad[p] = 1;
    } else if (pkind[p] == PG_SMALL) {
      stamp_n++;
      for (u32 i = p * 32; i < p * 32 + 32; i++) {
        u32 v = own[i];
        if (!v || stamp[v - 1] == stamp_n)
          continue;
        stamp[v - 1] = stamp_n;
        plist[k++] = (u16)v;
        if (wt[v - 1] == INF)
          bad[p] = 1;
      }
    }
    pstart[p + 1] = (u16)k;
    nbad += bad[p], nfresh += fresh[p];
    for (u32 j = pstart[p]; j < k; j++) {
      u32 v = plist[j];
      if (cnt[v - 1]++ == 0 && ecost[v - 1] != INF)
        sum += ecost[v - 1];
    }
    /* page p - n leaves it */
    if (p >= n) {
      u32 q = p - n;
      nbad -= bad[q], nfresh -= fresh[q];
      for (u32 j = pstart[q]; j < pstart[q + 1]; j++) {
        u32 v = plist[j];
        if (--cnt[v - 1] == 0 && ecost[v - 1] != INF)
          sum -= ecost[v - 1];
      }
    }
    if (p + 1 >= n && !nbad && (!max_pages || npages + nfresh <= max_pages) && sum < best) {
      best = sum, at = p + 1 - n;
      if (!best)
        break;
    }
  }
  /* the counts of the pages still in the window back to 0 */
  if (p == MX_VRAM_PAGES)
    p--;
  for (u32 q = p + 1 >= n ? p + 1 - n : 0; q <= p; q++)
    for (u32 j = pstart[q]; j < pstart[q + 1]; j++)
      cnt[plist[j] - 1] = 0;
  *cost = best;
  return at;
}

/* find_window(1): each page's cost alone */
static u32 find_page(u64 *cost) {
  const u8 *owner = mx__vram_owners();
  int fresh_ok = !max_pages || npages < max_pages;
  u64 best = INF;
  u32 at = ~0u;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    u64 c;
    if (pkind[p] == PG_BIG)
      c = ecost[pent[p] - 1];
    else if (pkind[p] == PG_SMALL)
      c = range_cost(p * 32, 32);
    else if (owner[p] == MX_VRAM_FREE && fresh_ok)
      c = 0;
    else
      continue;
    if (c < best) {
      best = c, at = p;
      if (!best)
        break;
    }
  }
  *cost = best;
  return at;
}

/* pages p .. p + n - 1 (free) for entry e as a large piece */
static u32 take_pages(u32 e, u32 p, u32 n) {
  for (u32 i = 0; i < n; i++)
    if (page_get(p + i, PG_BIG) != 0) {
      while (i--)
        page_drop(p + i);
      return ~0u;
    }
  blocks_set(p * 32, n * 32, e + 1);
  for (u32 i = 0; i < n; i++)
    pent[p + i] = (u16)(e + 1);
  return p * 32;
}

/* ---- ring order ------------------------------------------------------------ */

static int ring_on(void) { return policy == MX_VCACHE_RING; }

/* blocks b .. b + n - 1 hold no entry that must stay */
static int range_ok(u32 b, u32 n) {
  for (u32 i = b; i < b + n; i++)
    if (own[i] && weight(&ents[own[i] - 1u]) == INF)
      return 0;
  return 1;
}

/* the first run of n pages from ring_page on (round to page 0) that is
   free or the cache's without an entry that must stay, within max_pages */
static u32 ring_window(u32 n) {
  for (u32 k = 0; k < MX_VRAM_PAGES; k++) {
    u32 p = (ring_page + k) % MX_VRAM_PAGES, fresh = 0, i;
    if (p + n > MX_VRAM_PAGES)
      continue;
    for (i = 0; i < n; i++) {
      u32 o = mx_vram_owner(p + i);
      if (o != MX_VRAM_FREE && (o != MX_VRAM_CACHE || !range_ok((p + i) * 32, 32)))
        break;
      fresh += o == MX_VRAM_FREE;
    }
    if (i == n && (!max_pages || npages + fresh <= max_pages))
      return p;
  }
  return ~0u;
}

/* n pages in ring order: what is there goes */
static u32 ring_big(u32 e, u32 n) {
  u32 p = ring_window(n);
  if (p == ~0u)
    return ~0u;
  evict_range(p * 32, n * 32);
  ring_page = (p + n) % MX_VRAM_PAGES;
  st.ring_places++;
  return take_pages(e, p, n);
}

/* b < 32 blocks in ring order: after the last in the ring's page of small
   pieces, else from a page the ring takes next */
static u32 ring_small_place(u32 e, u32 b, u32 a) {
  u32 p = ring_small, s = (ring_blk + a - 1) & ~(a - 1);
  if (p == ~0u || pkind[p] != PG_SMALL || s + b > 32 || !range_ok(p * 32 + s, b)) {
    p = ring_window(1);
    if (p == ~0u)
      return ~0u;
    evict_range(p * 32, 32);
    ring_page = (p + 1) % MX_VRAM_PAGES;
    ring_small = p, s = 0;
  } else
    evict_range(p * 32 + s, b);
  /* emptied pages went back */
  if (pkind[p] == PG_NONE && page_get(p, PG_SMALL) != 0)
    return ~0u;
  ring_blk = s + b;
  st.ring_places++;
  blocks_set(p * 32 + s, b, e + 1);
  return p * 32 + s;
}

/* ---- placing --------------------------------------------------------------- */

/* n pages for entry e; returns the first block, or ~0u */
static u32 place_big(u32 e, u32 n) {
  u32 p = mx_vram_find(n);
  if (p == ~0u || (max_pages && npages + n > max_pages)) {
    if (ring_on())
      return ring_big(e, n);
    u64 c;
    weigh();
    p = find_window(n, &c);
    if (p == ~0u)
      return ~0u;
    evict_range(p * 32, n * 32);
  }
  return take_pages(e, p, n);
}

/* b <= 32 blocks for entry e, from a multiple of a (a power of two);
   returns the first block, or ~0u */
static u32 place_small(u32 e, u32 b, u32 a) {
  u32 m = b >= 32 ? ~0u : (1u << b) - 1, p, s;
  /* a free run in a page of small pieces */
  for (p = 0; p < MX_VRAM_PAGES; p++)
    if (pkind[p] == PG_SMALL)
      for (s = 0; s + b <= 32; s += a)
        if (!((mask[p] >> s) & m))
          goto take;
  /* a free page */
  if (!max_pages || npages < max_pages) {
    p = mx_vram_find(1);
    if (p != ~0u && page_get(p, PG_SMALL) == 0) {
      s = 0;
      goto take;
    }
  }
  if (ring_on())
    return ring_small_place(e, b, a);
  /* the cheapest run of a page of small pieces, or a page */
  weigh();
  u64 best = INF;
  u32 bp = ~0u, bs = 0;
  for (u32 q = 0; q < MX_VRAM_PAGES; q++)
    if (pkind[q] == PG_SMALL)
      for (u32 t = 0; t + b <= 32; t += a) {
        u64 c = range_cost(q * 32 + t, b);
        if (c < best)
          best = c, bp = q, bs = t;
      }
  u64 wc;
  u32 wp = find_window(1, &wc);
  if (wp != ~0u && wc < best) {
    evict_range(wp * 32, 32);
    p = wp, s = 0;
  } else if (bp != ~0u) {
    evict_range(bp * 32 + bs, b);
    p = bp, s = bs;
  } else
    return ~0u;
  /* emptied pages went back */
  if (pkind[p] == PG_NONE && page_get(p, PG_SMALL) != 0)
    return ~0u;
take:
  blocks_set(p * 32 + s, b, e + 1);
  return p * 32 + s;
}

/* a new entry for key (the cheapest one evicted if there is no free one) */
static int ent_new(u64 key) {
  if (!nfree) {
    weigh();
    u64 best = INF;
    u32 v = 0;
    for (u32 e = 0; e < ent_hi; e++)
      if (ecost[e] < best)
        best = ecost[e], v = e;
    if (best == INF)
      return -1;
    evict(v);
  }
  u32 e = free_ents[--nfree];
  if (e >= ent_hi)
    ent_hi = e + 1;
  struct ent *x = &ents[e];
  *x = (struct ent){.key = key, .last_use = now - 1, .live = 1, .building = 1};
  struct hist *h = &hist[hslot(key)];
  if (h->live && h->key == key && now - h->last_use <= 1) {
    x->last_use = h->last_use, x->uses = h->uses, x->uses_prev = h->uses_prev, x->freq = h->freq;
    h->live = 0;
  }
  wt[e] = ecost[e] = INF; /* building */
  for (u32 i = 0; i < NP; i++)
    x->bp[i] = ~0u;
  return (int)e;
}

static u32 pow2_of(u32 b) {
  u32 a = 1;
  while (a < b)
    a <<= 1;
  return a;
}

/* place the pieces of e (nb set), the largest first; the small ones as
   one run if they fit in a page (one search), each from a multiple of the
   power of two that holds it */
static int build(u32 e) {
  struct ent *x = &ents[e];
  u8 order[NP];
  for (u32 i = 0; i < x->npieces; i++) {
    u32 j = i;
    while (j > 0 && x->nb[order[j - 1]] < x->nb[i]) {
      order[j] = order[j - 1];
      j--;
    }
    order[j] = (u8)i;
  }
  u32 k = 0, off[NP], end = 0, nsm = 0;
  for (; k < x->npieces && x->nb[order[k]] >= 32; k++) {
    u32 i = order[k];
    x->bp[i] = place_big(e, x->nb[i] / 32);
    if (x->bp[i] == ~0u)
      goto fail;
  }
  for (u32 j = k; j < x->npieces; j++, nsm++) {
    u32 i = order[j], a = pow2_of(x->nb[i]);
    off[i] = (end + a - 1) & ~(a - 1);
    end = off[i] + x->nb[i];
  }
  if (nsm >= 2 && end <= 32) {
    u32 g = place_small(e, end, pow2_of(x->nb[order[k]]));
    if (g == ~0u)
      goto fail;
    blocks_set(g, end, 0); /* the gaps between pieces stay free */
    for (u32 j = k; j < x->npieces; j++) {
      u32 i = order[j];
      x->bp[i] = g + off[i];
      blocks_set(x->bp[i], x->nb[i], e + 1);
    }
  } else
    for (; k < x->npieces; k++) {
      u32 i = order[k];
      x->bp[i] = place_small(e, x->nb[i], pow2_of(x->nb[i]));
      if (x->bp[i] == ~0u)
        goto fail;
    }
  x->building = 0;
  h_insert(e);
  reweigh(e);
  return 0;
fail:
  ent_free(e);
  return -1;
}

static void use(struct ent *x) {
  if (x->last_use != now) {
    u32 age = now - x->last_use;
    x->freq = age > 8 ? 0 : (u8)(x->freq >> (age - 1));
    if (x->freq < 255)
      x->freq++;
    x->uses_prev = age == 1 ? x->uses : 0;
    x->uses = 0;
    x->last_use = now;
  }
  if (x->uses < 0xFFFF)
    x->uses++;
  x->useq = ++seq;
  if (wt_now == now)
    reweigh((u32)(x - ents));
}

static int indexed(u32 psm) {
  return psm == MX_GS_PSM_T8 || psm == MX_GS_PSM_T4 || psm == MX_GS_PSM_T8H ||
         psm == MX_GS_PSM_T4HL || psm == MX_GS_PSM_T4HH;
}

static void fill(const struct ent *x, struct mx_tex *t) {
  u32 psm = x->hn ? MX_GS_PSM_T8H : x->psm;
  *t = (struct mx_tex){.psm = (u8)psm, .levels = x->levels, .cpsm = MX_GS_PSM_CT32,
                       .blocks = x->blocks + x->hn};
  if (x->kind == K_BUF) {
    t->tbp[0] = x->bp[0];
    t->tbw[0] = (u8)mx_tex_tbw(x->w, x->psm);
    return;
  }
  t->tw = (u8)x->w, t->th = (u8)x->h;
  for (u32 n = 0; n < x->levels; n++) {
    t->tbp[n] = x->bp[n];
    t->tbw[n] = (u8)mx_tex_tbw(1u << (x->w > n ? x->w - n : 0), psm);
  }
  if (indexed(x->psm) && x->npieces > x->levels)
    t->cbp = x->bp[x->levels], t->cpsm = x->cpsm;
}

static u16 piece_blocks(u32 w, u32 h, u32 psm) {
  u32 b = mx_tex_blocks(w, h, psm);
  return (u16)(b >= 32 ? (b + 31) & ~31u : b);
}

/* the key's entry if it has this shape (a use: 1), else dropped (0), or -1
   if it cannot go */
static int lookup(u64 key, const struct ent *want, struct mx_tex *t) {
  int e = find(key);
  if (e < 0)
    return 0;
  struct ent *x = &ents[e];
  if (x->kind == want->kind && x->psm == want->psm && x->cpsm == want->cpsm && x->w == want->w &&
      x->h == want->h && x->levels == want->levels) {
    use(x);
    fill(x, t);
    st.hits++;
    return 1;
  }
  if (weight(x) == INF)
    return -1;
  ent_free((u32)e);
  return 0;
}

/* ---- the top bytes of 24-bit buffers ------------------------------------- */

/* The entries of top-byte page p, each once, into l (stamps): the number */
static u32 h_page_ents(u32 p, u16 *l) {
  if (h_uni[p]) {
    l[0] = hown[p * 32];
    return l[0] != 0;
  }
  u32 k = 0;
  stamp_n++;
  for (u32 i = p * 32; i < p * 32 + 32; i++) {
    u32 v = hown[i];
    if (v && stamp[v - 1] != stamp_n)
      stamp[v - 1] = stamp_n, l[k++] = (u16)v;
  }
  return k;
}

/* The run of len blocks from a multiple of align in the top bytes whose
   entries cost least (free blocks nothing); its first block (*cost), or
   ~0u. A page holds one entry or free blocks (h_uni) unless it has small
   pieces: a run of pages slides on a page at a time, reading a page as
   its entries (whole runs of pages: the first free one ends the search);
   a run within a page (len, align < 32) is looked for in the pages with
   small pieces, a free page first. */
static u32 h_find(u32 len, u32 align, u64 *cost) {
  u32 p0 = h_first / 32, pn = p0 + h_n / 32;
  *cost = INF;
  if (!h_n || len > h_n)
    return ~0u;
  weigh();
  u64 best = INF;
  u32 at = ~0u;
  if (align < 32 && len <= 32) {
    for (u32 p = p0; p < pn; p++) {
      if (h_uni[p]) {
        u32 v = hown[p * 32];
        u64 c = v ? ecost[v - 1] : 0;
        if (c < best && (!v || wt[v - 1] != INF))
          best = c, at = p * 32;
      } else
        for (u32 t = 0; t + len <= 32; t += align) {
          u64 c = 0;
          stamp_n++;
          for (u32 i = p * 32 + t; i < p * 32 + t + len && c != INF; i++) {
            u32 v = hown[i];
            if (!v || stamp[v - 1] == stamp_n)
              continue;
            stamp[v - 1] = stamp_n;
            c = wt[v - 1] == INF ? INF : c + ecost[v - 1];
          }
          if (c < best)
            best = c, at = p * 32 + t;
        }
      if (!best)
        break;
    }
    *cost = best;
    return at;
  }
  /* k pages: the window's entries counted once (cnt), its sum, and the
     pages in it with an entry that must stay */
  static u16 plist[MX_VRAM_BLOCKS], pstart[MX_VRAM_PAGES + 1];
  static u8 bad[MX_VRAM_PAGES];
  u32 k = (len + 31) / 32, n = 0, nbad = 0, p;
  u64 sum = 0;
  if (p0 + k > pn)
    return ~0u;
  for (p = p0; p < pn; p++) {
    pstart[p] = (u16)n;
    u32 m = h_page_ents(p, plist + n);
    bad[p] = 0;
    for (u32 j = n; j < n + m; j++) {
      u32 v = plist[j];
      if (wt[v - 1] == INF)
        bad[p] = 1;
      else if (cnt[v - 1]++ == 0)
        sum += ecost[v - 1];
    }
    n += m;
    pstart[p + 1] = (u16)n;
    nbad += bad[p];
    if (p >= p0 + k) {
      u32 q = p - k;
      nbad -= bad[q];
      for (u32 j = pstart[q]; j < pstart[q + 1]; j++) {
        u32 v = plist[j];
        if (wt[v - 1] != INF && --cnt[v - 1] == 0)
          sum -= ecost[v - 1];
      }
    }
    if (p + 1 >= p0 + k && !nbad && sum < best) {
      best = sum, at = (p + 1 - k) * 32;
      if (!best)
        break;
    }
  }
  /* the counts of the pages still in the window back to 0 */
  if (p == pn)
    p--;
  for (u32 q = p + 1 >= p0 + k ? p + 1 - k : p0; q <= p; q++)
    for (u32 j = pstart[q]; j < pstart[q + 1]; j++)
      cnt[plist[j] - 1] = 0;
  *cost = best;
  return at;
}

/* what placing n blocks in pages of the cache costs (an estimate: the
   cheapest run for a large piece, the cheapest page for a small one) */
static u64 page_cost(u32 n) {
  u64 c;
  if (n >= 32) {
    u32 k = n / 32;
    if (mx_vram_find(k) != ~0u && (!max_pages || npages + k <= max_pages))
      return 0;
    return find_window(k, &c) == ~0u ? INF : c;
  }
  u32 a = pow2_of(n), m = (1u << n) - 1;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++)
    if (pkind[p] == PG_SMALL)
      for (u32 s = 0; s + n <= 32; s += a)
        if (!((mask[p] >> s) & m))
          return 0;
  if ((!max_pages || npages < max_pages) && mx_vram_find(1) != ~0u)
    return 0;
  return find_page(&c) == ~0u ? INF : c;
}

/* Entry e (a T8 texture being built) in the top bytes as T8H if they have
   room that costs less than pages of the cache: its levels there (laid out
   as mx_tex_init), its CLUT in a page. 1 placed, 0 not (build it as
   usual), -1 placed nowhere. */
static int h_try(u32 e) {
  struct ent *x = &ents[e];
  if (!h_n || x->kind != K_TEX || x->psm != MX_GS_PSM_T8)
    return 0;
  struct mx_tex lay;
  u32 len = mx_tex_init(&lay, 0, MX_GS_PSM_T8H, x->w, x->h, x->levels, MX_TEX_PACKED);
  u32 l0 = mx_tex_blocks(1u << x->w, 1u << x->h, MX_GS_PSM_T8H);
  u32 align = l0 >= 32 ? 32 : pow2_of(l0);
  u64 c;
  u32 s = h_find(len, align, &c);
  if (s == ~0u || (c && c >= page_cost(x->nb[0])))
    return 0;
  for (u32 b = s; b < s + len; b++)
    st.evict_run_blocks += hown[b] != 0;
  for (u32 b = s; b < s + len; b++)
    if (hown[b])
      evict(hown[b] - 1u);
  for (u32 b = s; b < s + len; b++)
    hown[b] = (u16)(e + 1);
  h_note(s, len);
  h_used += len;
  x->hb = s, x->hn = (u16)len;
  for (u32 n = 0; n < x->levels; n++)
    x->bp[n] = s + lay.tbp[n];
  x->blocks = 0;
  if (x->npieces > x->levels) {
    u32 i = x->levels;
    x->bp[i] = place_small(e, x->nb[i], pow2_of(x->nb[i]));
    if (x->bp[i] == ~0u) {
      ent_free(e);
      return -1;
    }
    x->blocks = x->nb[i];
  }
  x->building = 0;
  h_insert(e);
  reweigh(e);
  st.hbyte_places++;
  return 1;
}

/* key is not there (lookup gave 0): placed as want */
static int place_new(u64 key, struct ent *want, struct mx_tex *t) {
  int e = ent_new(key);
  if (e < 0) {
    st.full++;
    return MX_VCACHE_FULL;
  }
  struct ent *x = &ents[e];
  x->kind = want->kind, x->psm = want->psm, x->cpsm = want->cpsm, x->w = want->w, x->h = want->h;
  x->levels = want->levels, x->npieces = want->npieces;
  x->blocks = 0;
  for (u32 i = 0; i < x->npieces; i++)
    x->nb[i] = want->nb[i], x->blocks += want->nb[i];
  int r = h_try((u32)e);
  if (r < 0 || (r == 0 && build((u32)e) != 0)) {
    st.full++;
    return MX_VCACHE_FULL;
  }
  use(x);
  fill(x, t);
  st.misses++;
  st.upload_blocks += x->blocks + x->hn / 4u;
  return MX_VCACHE_MISS;
}

static int lookup_or_place(u64 key, struct ent *want, struct mx_tex *t) {
  int r = lookup(key, want, t);
  if (r > 0)
    return MX_VCACHE_HIT;
  if (r < 0) {
    st.full++;
    return MX_VCACHE_FULL;
  }
  return place_new(key, want, t);
}

/* ---- the interface --------------------------------------------------------- */

void mx_vcache_reset(void) {
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    /* mx_vram_reset may have given the page to another owner since */
    if (pkind[p] != PG_NONE && mx_vram_owner(p) == MX_VRAM_CACHE)
      mx_vram_give(p, 1);
    pkind[p] = PG_NONE, mask[p] = 0, pent[p] = 0;
  }
  for (u32 i = 0; i < MX_VRAM_BLOCKS; i++)
    own[i] = 0, hown[i] = 0;
  h_used = 0;
  for (u32 i = 0; i < NH; i++)
    htab[i] = 0;
  nfree = 0;
  for (u32 e = NE; e > 0; e--) {
    ents[e - 1].live = 0;
    free_ents[nfree++] = (u16)(e - 1);
  }
  npages = nsmall = used_blocks = ent_hi = 0;
  wt_now = ~0u;
  ring_page = ring_blk = 0, ring_small = ~0u;
  for (u32 i = 0; i < NHIST; i++)
    hist[i].live = 0;
}

void mx_vcache_init(u32 maxp, int m, int pol) {
  inits++;
  mx_vcache_reset();
  max_pages = maxp, mode = (u32)m, policy = (u32)pol;
  h_first = h_n = 0;
  st = (struct mx_vcache_stats){0};
}

void mx_vcache_hpages(u32 page, u32 pages) {
  for (u32 b = h_first; b < h_first + h_n; b++)
    if (hown[b])
      ent_free(hown[b] - 1u);
  h_first = page * 32, h_n = pages * 32;
  for (u32 p = page; p < page + pages; p++)
    h_uni[p] = 1;
}

void mx_vcache_frame(u32 n, u32 d) {
  if (n != now)
    seq0_prev = seq0, seq0 = seq;
  now = n, done = d;
  wt_now = ~0u;
}
u32 mx__vcache_now(void) { return now; }

/* Debugging: the places against the owner maps. Returns 0, or prints the
   first fault and returns 1: a piece's block owned by another entry (or
   none), an entry's block that is not its own page's kind, a block owned
   by an entry that is not live. */
int mx__vcache_check(void) {
  for (u32 e = 0; e < ent_hi; e++) {
    const struct ent *x = &ents[e];
    if (!x->live || x->building)
      continue;
    for (u32 i = 0; i < x->npieces; i++) {
      int in_h = x->hn && i < x->levels;
      u32 b = x->bp[i], n = in_h ? 0 : x->nb[i];
      for (u32 k = b; k < b + n; k++)
        if (own[k] != e + 1) {
          mx_printf("vcache check: entry %u (key %08x%08x) piece %u block %u owned by %u\n", e,
                    (u32)(x->key >> 32), (u32)x->key, i, k, own[k]);
          return 1;
        }
      if (n && (n >= 32 ? pkind[b / 32] != PG_BIG : pkind[b / 32] != PG_SMALL)) {
        mx_printf("vcache check: entry %u piece %u at %u (%u blocks): page kind %u\n", e, i, b, n,
                  pkind[b / 32]);
        return 1;
      }
    }
    for (u32 k = x->hb; x->hn && k < x->hb + x->hn; k++)
      if (hown[k] != e + 1) {
        mx_printf("vcache check: entry %u top byte block %u owned by %u\n", e, k, hown[k]);
        return 1;
      }
  }
  for (u32 k = 0; k < MX_VRAM_BLOCKS; k++)
    if ((own[k] && !ents[own[k] - 1].live) || (hown[k] && !ents[hown[k] - 1].live)) {
      mx_printf("vcache check: block %u owned by a free entry\n", k);
      return 1;
    }
  return 0;
}
u32 mx__vcache_inits(void) { return inits; }
void mx__vcache_count(int upgrade) { upgrade ? st.upgrades++ : st.proxies++; }

int mx_vcache_tex(u64 key, u32 psm, u32 tw, u32 th, u32 levels, u32 cpsm, struct mx_tex *t) {
  if (levels < 1)
    levels = 1;
  if (levels > MX_TEX_LEVELS)
    levels = MX_TEX_LEVELS;
  struct ent want = {.kind = K_TEX, .psm = (u8)psm, .w = (u16)tw, .h = (u16)th,
                     .levels = (u8)levels, .npieces = (u8)levels};
  int clut = indexed(psm) && cpsm != MX_VCACHE_NO_CLUT;
  if (indexed(psm))
    want.cpsm = (u8)cpsm; /* MX_VCACHE_NO_CLUT: not the same shape as one with a CLUT */
  /* a hit needs the shape alone; the pieces' sizes only a miss */
  int r = lookup(key, &want, t);
  if (r > 0)
    return MX_VCACHE_HIT;
  if (r < 0) {
    st.full++;
    return MX_VCACHE_FULL;
  }
  for (u32 n = 0; n < levels; n++)
    want.nb[n] = piece_blocks(1u << (tw > n ? tw - n : 0), 1u << (th > n ? th - n : 0), psm);
  if (clut) {
    int t4 = psm == MX_GS_PSM_T4 || psm == MX_GS_PSM_T4HL || psm == MX_GS_PSM_T4HH;
    want.nb[want.npieces++] = piece_blocks(t4 ? 8 : 16, t4 ? 2 : 16, cpsm);
  }
  return place_new(key, &want, t);
}

int mx_vcache_buf(u64 key, u32 w, u32 h, u32 psm, u32 *bp) {
  struct ent want = {.kind = K_BUF, .psm = (u8)psm, .w = (u16)w, .h = (u16)h, .levels = 1,
                     .npieces = 1};
  want.nb[0] = piece_blocks(w, h, psm);
  struct mx_tex t;
  int r = lookup_or_place(key, &want, &t);
  if (r != MX_VCACHE_FULL)
    *bp = t.tbp[0];
  return r;
}

int mx_vcache_use(u64 key) {
  int e = find(key);
  if (e < 0)
    return -1;
  use(&ents[e]);
  return 0;
}

int mx_vcache_peek(u64 key, struct mx_tex *t) {
  int e = find(key);
  if (e < 0)
    return 0;
  fill(&ents[e], t);
  return 1;
}

int mx_vcache_pin(u64 key) {
  int e = find(key);
  if (e < 0)
    return -1;
  ents[e].pins++;
  reweigh((u32)e);
  return 0;
}

void mx_vcache_unpin(u64 key) {
  int e = find(key);
  if (e >= 0 && ents[e].pins)
    ents[e].pins--, reweigh((u32)e);
}

int mx_vcache_drop(u64 key) {
  int e = find(key);
  if (e < 0 || weight(&ents[e]) == INF)
    return -1;
  ent_free((u32)e);
  return 0;
}

void mx_vcache_stats(struct mx_vcache_stats *s) {
  *s = st;
  s->entries = NE - nfree;
  s->pages = npages;
  s->small_pages = nsmall;
  s->used_blocks = used_blocks;
  s->hbyte_blocks = h_n;
  s->hbyte_used = h_used;
}

void mx_vcache_space(struct mx_vcache_space *s) {
  *s = (struct mx_vcache_space){0};
  /* where the blocks go (measured, for the packing): each piece's texels
     (whole blocks of 256 bytes; T8H 64 texels), its span in the page's
     block order (mx_tex_blocks), its blocks (a large one: whole pages) */
  for (u32 e = 0; e < ent_hi; e++) {
    const struct ent *x = &ents[e];
    if (!x->live || x->building)
      continue;
    for (u32 i = 0; i < x->npieces; i++) {
      u32 nb = x->nb[i], need = nb, span = nb;
      if (x->kind == K_TEX && i < x->levels) {
        u32 w = 1u << (x->w > i ? x->w - i : 0), h = 1u << (x->h > i ? x->h - i : 0);
        u32 bits = x->psm == MX_GS_PSM_T4 ? 4 : x->psm == MX_GS_PSM_T8 ? 8
                 : x->psm == MX_GS_PSM_CT16 || x->psm == MX_GS_PSM_CT16S ? 16 : 32;
        if (x->hn) {
          s->hbyte_need += (w * h + 63) / 64;
          continue;
        }
        need = (w * h * bits / 8 + 255) / 256;
        span = mx_tex_blocks(w, h, x->psm);
      }
      s->need_blocks += need;
      s->span_waste += span - need;
      s->round_waste += nb - span;
    }
  }
  for (u32 p = 0; p < MX_VRAM_PAGES; p++)
    if (pkind[p] == PG_SMALL)
      s->small_free += 32 - (u32)__builtin_popcount(mask[p]);
}

