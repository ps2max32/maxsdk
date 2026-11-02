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

#define NE MX_VCACHE_ENTRIES
#define NH (2 * NE) /* hash slots, a power of two */
#define NP (MX_TEX_LEVELS + 1)
#define INF (~0ull)
#define NOW_BASE (1ull << 22) /* above every weight of an older entry */

enum { K_TEX, K_BUF };
enum { PG_NONE, PG_BIG, PG_SMALL };

struct ent {
  u64 key;
  u32 last_use;   /* frame */
  u32 useq;       /* the use counter at its last use */
  u32 bp[NP];     /* each piece's first block (~0u: not placed) */
  u16 nb[NP];     /* each piece's blocks */
  u16 w, h;       /* K_TEX: tw, th; K_BUF: pixels */
  u16 blocks;     /* all pieces */
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
static u32 policy;
/* ring order: the page it goes on from, the page of small pieces it fills
   and the block there; the frame AUTO went to ring order in */
static u32 ring_page, ring_small = ~0u, ring_blk, ring_frame = ~0u;
static struct mx_vcache_stats st;
/* while making room */
static u64 wt[NE], ecost[NE];
static u32 cnt[NE], stamp[NE], stamp_n;

/* ---- the key -> entry hash (linear probing, deletion by backward shift) --- */

static u32 hash(u64 k) { return (u32)((k * 0x9E3779B97F4A7C15ull) >> 32) & (NH - 1); }

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
  pkind[p] = PG_NONE, mask[p] = 0;
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
  for (u32 i = 0; i < x->npieces; i++) {
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
  free_ents[nfree++] = (u16)e;
}

static u64 weight(const struct ent *x) {
  if (x->pins || x->building)
    return INF;
  u32 age = now - x->last_use;
  if (mode == MX_VCACHE_ASYNC && (s32)(done - x->last_use) <= 0)
    return INF;
  if (age == 0) {
    if (mode != MX_VCACHE_STREAM)
      return INF;
    /* used this frame: the most recently used goes first */
    u32 d = seq - x->useq;
    return NOW_BASE + (d > 0xFFFF ? 0xFFFF : d);
  }
  u32 sh = age - 1 > 20 ? 20 : age - 1;
  u64 w = ((u64)(16 + x->freq) << 12) >> sh;
  return w ? w : 1;
}

static void evict(u32 e) {
  st.evicted++;
  st.evicted_now += ents[e].last_use == now;
  ent_free(e);
}

static void weigh(void) {
  for (u32 e = 0; e < ent_hi; e++) {
    wt[e] = ents[e].live ? weight(&ents[e]) : 0;
    ecost[e] = wt[e] == INF ? INF : wt[e] * ents[e].blocks;
  }
}

static void evict_range(u32 b, u32 n) {
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
   (*cost) or ~0u. Needs weigh(). */
static u32 find_window(u32 n, u64 *cost) {
  static u8 bad[MX_VRAM_PAGES], fresh[MX_VRAM_PAGES];
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    u32 o = mx_vram_owner(p);
    fresh[p] = o == MX_VRAM_FREE;
    bad[p] = o != MX_VRAM_FREE && o != MX_VRAM_CACHE;
    for (u32 i = 0; i < 32 && !bad[p] && pkind[p] != PG_NONE; i++)
      bad[p] = own[p * 32 + i] && wt[own[p * 32 + i] - 1u] == INF;
  }
  for (u32 e = 0; e < ent_hi; e++)
    cnt[e] = 0;
  u64 sum = 0, best = INF;
  u32 at = ~0u, nbad = 0, nfresh = 0;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    /* page p comes into the window, page p - n leaves it */
    nbad += bad[p], nfresh += fresh[p];
    stamp_n++;
    for (u32 i = p * 32; i < p * 32 + 32 && pkind[p] != PG_NONE; i++) {
      u32 v = own[i];
      if (v && stamp[v - 1] != stamp_n) {
        stamp[v - 1] = stamp_n;
        if (cnt[v - 1]++ == 0 && ecost[v - 1] != INF)
          sum += ecost[v - 1];
      }
    }
    if (p >= n) {
      u32 q = p - n;
      nbad -= bad[q], nfresh -= fresh[q];
      stamp_n++;
      for (u32 i = q * 32; i < q * 32 + 32 && pkind[q] != PG_NONE; i++) {
        u32 v = own[i];
        if (v && stamp[v - 1] != stamp_n) {
          stamp[v - 1] = stamp_n;
          if (--cnt[v - 1] == 0 && ecost[v - 1] != INF)
            sum -= ecost[v - 1];
        }
      }
    }
    if (p + 1 >= n && !nbad && (!max_pages || npages + nfresh <= max_pages) && sum < best) {
      best = sum, at = p + 1 - n;
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
  return p * 32;
}

/* ---- ring order ------------------------------------------------------------ */

static int ring_on(void) {
  return policy == MX_VCACHE_RING || (policy == MX_VCACHE_AUTO && ring_frame == now);
}

/* blocks b .. b + n - 1 hold no entry that must stay */
static int range_ok(u32 b, u32 n) {
  for (u32 i = b; i < b + n; i++)
    if (own[i] && weight(&ents[own[i] - 1u]) == INF)
      return 0;
  return 1;
}

/* AUTO: making room would evict an entry this frame used (its textures do
   not all fit): the rest of the frame places in ring order */
static int thrash(u32 b, u32 n) {
  if (policy != MX_VCACHE_AUTO || mode != MX_VCACHE_STREAM)
    return 0;
  for (u32 i = b; i < b + n; i++)
    if (own[i] && ents[own[i] - 1u].last_use == now) {
      ring_frame = now;
      st.ring_frames++;
      return 1;
    }
  return 0;
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
    if (thrash(p * 32, n * 32))
      return ring_big(e, n);
    evict_range(p * 32, n * 32);
  }
  return take_pages(e, p, n);
}

/* b < 32 blocks for entry e, from a multiple of the power of two that
   holds b; returns the first block, or ~0u */
static u32 place_small(u32 e, u32 b) {
  u32 a = 1;
  while (a < b)
    a <<= 1;
  u32 m = (1u << b) - 1, p, s;
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
    if (thrash(wp * 32, 32))
      return ring_small_place(e, b, a);
    evict_range(wp * 32, 32);
    p = wp, s = 0;
  } else if (bp != ~0u) {
    if (thrash(bp * 32 + bs, b))
      return ring_small_place(e, b, a);
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
  for (u32 i = 0; i < NP; i++)
    x->bp[i] = ~0u;
  return (int)e;
}

/* place the pieces of e (nb set), the largest first */
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
  for (u32 k = 0; k < x->npieces; k++) {
    u32 i = order[k], n = x->nb[i];
    x->bp[i] = n >= 32 ? place_big(e, n / 32) : place_small(e, n);
    if (x->bp[i] == ~0u) {
      ent_free(e);
      return -1;
    }
  }
  x->building = 0;
  h_insert(e);
  return 0;
}

static void use(struct ent *x) {
  if (x->last_use != now) {
    u32 age = now - x->last_use;
    x->freq = age > 8 ? 0 : (u8)(x->freq >> (age - 1));
    if (x->freq < 255)
      x->freq++;
    x->last_use = now;
  }
  x->useq = ++seq;
}

static int indexed(u32 psm) {
  return psm == MX_GS_PSM_T8 || psm == MX_GS_PSM_T4 || psm == MX_GS_PSM_T8H ||
         psm == MX_GS_PSM_T4HL || psm == MX_GS_PSM_T4HH;
}

static void fill(const struct ent *x, struct mx_tex *t) {
  *t = (struct mx_tex){.psm = x->psm, .levels = x->levels, .cpsm = MX_GS_PSM_CT32,
                       .blocks = x->blocks};
  if (x->kind == K_BUF) {
    t->tbp[0] = x->bp[0];
    t->tbw[0] = (u8)mx_tex_tbw(x->w, x->psm);
    return;
  }
  t->tw = (u8)x->w, t->th = (u8)x->h;
  for (u32 n = 0; n < x->levels; n++) {
    t->tbp[n] = x->bp[n];
    t->tbw[n] = (u8)mx_tex_tbw(1u << (x->w > n ? x->w - n : 0), x->psm);
  }
  if (indexed(x->psm))
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

static int lookup_or_place(u64 key, struct ent *want, struct mx_tex *t) {
  int r = lookup(key, want, t);
  if (r > 0)
    return MX_VCACHE_HIT;
  if (r < 0) {
    st.full++;
    return MX_VCACHE_FULL;
  }
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
  if (build((u32)e) != 0) {
    st.full++;
    return MX_VCACHE_FULL;
  }
  use(x);
  fill(x, t);
  st.misses++;
  st.upload_blocks += x->blocks;
  return MX_VCACHE_MISS;
}

/* ---- the interface --------------------------------------------------------- */

void mx_vcache_reset(void) {
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    /* mx_vram_reset may have given the page to another owner since */
    if (pkind[p] != PG_NONE && mx_vram_owner(p) == MX_VRAM_CACHE)
      mx_vram_give(p, 1);
    pkind[p] = PG_NONE, mask[p] = 0;
  }
  for (u32 i = 0; i < MX_VRAM_BLOCKS; i++)
    own[i] = 0;
  for (u32 i = 0; i < NH; i++)
    htab[i] = 0;
  nfree = 0;
  for (u32 e = NE; e > 0; e--) {
    ents[e - 1].live = 0;
    free_ents[nfree++] = (u16)(e - 1);
  }
  npages = nsmall = used_blocks = ent_hi = 0;
  ring_page = ring_blk = 0, ring_small = ring_frame = ~0u;
}

void mx_vcache_init(u32 maxp, int m, int pol) {
  mx_vcache_reset();
  max_pages = maxp, mode = (u32)m, policy = (u32)pol;
  st = (struct mx_vcache_stats){0};
}

void mx_vcache_frame(u32 n, u32 d) { now = n, done = d; }

int mx_vcache_tex(u64 key, u32 psm, u32 tw, u32 th, u32 levels, u32 cpsm, struct mx_tex *t) {
  if (levels < 1)
    levels = 1;
  if (levels > MX_TEX_LEVELS)
    levels = MX_TEX_LEVELS;
  struct ent want = {.kind = K_TEX, .psm = (u8)psm, .w = (u16)tw, .h = (u16)th,
                     .levels = (u8)levels, .npieces = (u8)levels};
  for (u32 n = 0; n < levels; n++)
    want.nb[n] = piece_blocks(1u << (tw > n ? tw - n : 0), 1u << (th > n ? th - n : 0), psm);
  if (indexed(psm)) {
    int t4 = psm == MX_GS_PSM_T4 || psm == MX_GS_PSM_T4HL || psm == MX_GS_PSM_T4HH;
    want.cpsm = (u8)cpsm;
    want.nb[want.npieces++] = piece_blocks(t4 ? 8 : 16, t4 ? 2 : 16, cpsm);
  }
  return lookup_or_place(key, &want, t);
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
  return 0;
}

void mx_vcache_unpin(u64 key) {
  int e = find(key);
  if (e >= 0 && ents[e].pins)
    ents[e].pins--;
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
}
