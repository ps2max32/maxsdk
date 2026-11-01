/* malloc, free, realloc, calloc, memalign for libmxc, in place of
   picolibc's: that one clears every block it hands out and keeps one free
   list sorted by address, walked from its start by every free. With
   virtual memory (<mx/vm.h>) the heap is VM and each touch of a page may
   be a swap file transfer: a 16 MiB malloc wrote all 16 MiB, and each
   free read headers all over the heap (Quake's loading thrashed).

   Here (TLSF, two-level segregated fit): free blocks are kept in lists by
   size class, found through two bitmaps; malloc and free touch the block's
   header, its neighbours' and the head of a list, nothing else. malloc does
   not clear (memory new from sbrk is zero anyway: VM pages are zero-filled
   when first touched); calloc does.

   A block: a 16-byte header, then the payload (16-byte aligned).
   - prev_size: the size of the block before it in memory;
   - size: this block's size (header included, a multiple of 16) | USED;
   - next, prev: a free block's neighbours in its list, as offsets from the
     heap's first address (0: none), so the header stays 16 bytes on a
     64-bit host too (tests/host/test_malloc.c).
   Each region from sbrk starts with a used 16-byte block and ends with a
   used header of size 0, so coalescing never looks past it; regions that
   follow each other merge. Not for interrupt handlers. */

#ifdef MXC_MALLOC_TEST /* tests/host/test_malloc.c: beside the host's own */
#define malloc mxc_malloc
#define free mxc_free
#define calloc mxc_calloc
#define realloc mxc_realloc
#define memalign mxc_memalign
#define aligned_alloc mxc_aligned_alloc
#define posix_memalign mxc_posix_memalign
#define valloc mxc_valloc
#define pvalloc mxc_pvalloc
#define malloc_usable_size mxc_malloc_usable_size
#endif

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint32_t u32;

#define HDR 16u
#define USED 1u
#define MIN_GROW (64u * 1024u)

struct blk {
  u32 prev_size;
  u32 size; /* | USED */
  u32 next, prev;
};

void *mxc_morecore(intptr_t incr); /* sbrk: the target's, or the test's */

/* ---- size classes --------------------------------------------------------------- */

#define SL_BITS 4
#define SL_N (1u << SL_BITS)
#define FL_N 25 /* sizes up to 2^31 */
#define SMALL 256u /* below: class 0, 16 lists of 16 bytes */

static u32 fl_map, sl_map[FL_N];
static u32 heads[FL_N][SL_N];
static char *base; /* offsets count from here */

static int msb(u32 v) { return 31 - __builtin_clz(v); }

static void mapping(u32 size, u32 *fl, u32 *sl) {
  if (size < SMALL) {
    *fl = 0, *sl = size >> 4;
  } else {
    int m = msb(size);
    *fl = (u32)m - 7;
    *sl = (size >> (m - SL_BITS)) - SL_N;
  }
}

/* the class whose every block holds size */
static void mapping_search(u32 size, u32 *fl, u32 *sl) {
  if (size >= SMALL)
    size += (1u << (msb(size) - SL_BITS)) - 1;
  mapping(size, fl, sl);
}

static struct blk *at(u32 off) { return off ? (struct blk *)(base + off) : NULL; }
static u32 off_of(struct blk *b) { return (u32)((char *)b - base); }
static u32 bsize(const struct blk *b) { return b->size & ~USED; }
static struct blk *next_blk(struct blk *b) { return (struct blk *)((char *)b + bsize(b)); }
static struct blk *prev_blk(struct blk *b) { return (struct blk *)((char *)b - b->prev_size); }

static void unlink_free(struct blk *b) {
  u32 fl, sl;
  mapping(bsize(b), &fl, &sl);
  if (b->prev)
    at(b->prev)->next = b->next;
  else
    heads[fl][sl] = b->next;
  if (b->next)
    at(b->next)->prev = b->prev;
  if (!heads[fl][sl]) {
    sl_map[fl] &= ~(1u << sl);
    if (!sl_map[fl])
      fl_map &= ~(1u << fl);
  }
}

static void link_free(struct blk *b) {
  u32 fl, sl;
  mapping(bsize(b), &fl, &sl);
  b->size = bsize(b);
  b->prev = 0;
  b->next = heads[fl][sl];
  if (b->next)
    at(b->next)->prev = off_of(b);
  heads[fl][sl] = off_of(b);
  sl_map[fl] |= 1u << sl;
  fl_map |= 1u << fl;
}

/* a free block of at least size, taken out of its list, or NULL */
static struct blk *find(u32 size) {
  u32 fl, sl;
  mapping_search(size, &fl, &sl);
  if (fl >= FL_N)
    return NULL;
  u32 m = sl_map[fl] & (~0u << sl);
  if (!m) {
    u32 f = fl + 1 < 32 ? fl_map & (~0u << (fl + 1)) : 0;
    if (!f)
      return NULL;
    fl = (u32)__builtin_ctz(f);
    m = sl_map[fl];
  }
  sl = (u32)__builtin_ctz(m);
  struct blk *b = at(heads[fl][sl]);
  unlink_free(b);
  return b;
}

/* b (used, not in a list) cut to size; the rest freed */
static void trim(struct blk *b, u32 size);
static void release(struct blk *b);

/* ---- the heap --------------------------------------------------------------------- */

static struct blk *epilogue; /* the end of the last region */

/* more memory, at least size bytes in one free block (in its list) */
static int grow(u32 size) {
  /* find() looks in the class above size's, from the class's smallest
     size: a block of just size may be in the class below */
  if (size >= SMALL)
    size += 1u << (msb(size) - SL_BITS);
  /* a free last block is extended: only what it lacks (memory runs out at
     the top first) */
  u32 have = 0;
  if (epilogue && epilogue->prev_size) {
    struct blk *last = prev_blk(epilogue);
    if (!(last->size & USED))
      have = bsize(last);
  }
  u32 want = size > have ? size - have : 0;
  u32 incr = want + 2 * HDR > MIN_GROW ? want + 2 * HDR : MIN_GROW;
  incr = (incr + 15) & ~15u;
  char *p = mxc_morecore((intptr_t)incr);
  if (p == (char *)-1)
    return 0;
  struct blk *b;
  if (!base) {
    base = p;
  }
  if (epilogue && (char *)epilogue + HDR == p) {
    /* right after the last region: its end header becomes the block */
    b = epilogue;
    b->size = incr;
  } else {
    if ((uintptr_t)p & 15 || p < base)
      return 0; /* not ours to use */
    struct blk *pro = (struct blk *)p;
    pro->prev_size = 0;
    pro->size = HDR | USED;
    b = (struct blk *)(p + HDR);
    b->prev_size = HDR;
    b->size = incr - 2 * HDR;
  }
  epilogue = (struct blk *)(p + incr - HDR);
  epilogue->prev_size = bsize(b);
  epilogue->size = USED;
  b->size |= USED;
  release(b);
  return 1;
}

/* b freed: merged with free neighbours, into its list */
static void release(struct blk *b) {
  b->size &= ~USED;
  struct blk *n = next_blk(b);
  if (!(n->size & USED)) {
    unlink_free(n);
    b->size += bsize(n);
  }
  if (b->prev_size) {
    struct blk *p = prev_blk(b);
    if (!(p->size & USED)) {
      unlink_free(p);
      p->size = bsize(p) + bsize(b);
      b = p;
    }
  }
  next_blk(b)->prev_size = bsize(b);
  link_free(b);
}

static void trim(struct blk *b, u32 size) {
  u32 rest = bsize(b) - size;
  if (rest < HDR)
    return;
  b->size = size | USED;
  struct blk *r = next_blk(b);
  r->prev_size = size;
  r->size = rest | USED;
  next_blk(r)->prev_size = rest;
  release(r);
}

static u32 need(size_t n, int *bad) {
  if (n > 0x7FFF0000u) {
    *bad = 1;
    return 0;
  }
  u32 s = ((u32)n + 15) & ~15u;
  return (s ? s : 16) + HDR;
}

/* ---- the lock: one thread at a time (BIOS semaphore, on the PS2) -------------------- */

#ifdef MXC_MALLOC_TEST
static void lock(void) {}
static void unlock(void) {}
#else
#include <mx/thread.h>
static int sema = -1;
static void lock(void) {
  if (sema < 0)
    sema = mx_sema_create(1, 1);
  mx_sema_wait(sema);
}
static void unlock(void) { mx_sema_signal(sema); }
void *mx_sbrk(int32_t incr);
void *mxc_morecore(intptr_t incr) { return mx_sbrk((int32_t)incr); }
#endif

/* ---- the interface ------------------------------------------------------------------- */

/* the first block of size's own class that holds it (find() skips that
   class): a walk of one list, only before the heap grows */
static struct blk *first_fit(u32 size) {
  u32 fl, sl;
  mapping(size, &fl, &sl);
  if (fl >= FL_N)
    return NULL;
  for (u32 o = heads[fl][sl]; o; o = at(o)->next)
    if (bsize(at(o)) >= size) {
      struct blk *b = at(o);
      unlink_free(b);
      return b;
    }
  return NULL;
}

static void *alloc(u32 size) {
  struct blk *b = find(size);
  if (!b)
    b = first_fit(size);
  if (!b) {
    if (!grow(size))
      return NULL;
    b = find(size);
    if (!b)
      return NULL;
  }
  b->size |= USED;
  trim(b, size);
  return (char *)b + HDR;
}

void *malloc(size_t n) {
  int bad = 0;
  u32 size = need(n, &bad);
  if (bad) {
    errno = ENOMEM;
    return NULL;
  }
  lock();
  void *p = alloc(size);
  unlock();
  if (!p)
    errno = ENOMEM;
  return p;
}

void free(void *p) {
  if (!p)
    return;
  lock();
  release((struct blk *)((char *)p - HDR));
  unlock();
}

void *calloc(size_t n, size_t m) {
  if (m && n > (size_t)-1 / m) {
    errno = ENOMEM;
    return NULL;
  }
  void *p = malloc(n * m);
  if (p)
    memset(p, 0, n * m);
  return p;
}

size_t malloc_usable_size(void *p) {
  return p ? bsize((struct blk *)((char *)p - HDR)) - HDR : 0;
}

void *realloc(void *p, size_t n) {
  if (!p)
    return malloc(n);
  if (!n) {
    free(p);
    return NULL;
  }
  int bad = 0;
  u32 size = need(n, &bad);
  if (bad) {
    errno = ENOMEM;
    return NULL;
  }
  struct blk *b = (struct blk *)((char *)p - HDR);
  lock();
  if (bsize(b) < size) {
    struct blk *nx = next_blk(b);
    if (!(nx->size & USED) && bsize(b) + bsize(nx) >= size) {
      unlink_free(nx);
      b->size = (bsize(b) + bsize(nx)) | USED;
      next_blk(b)->prev_size = bsize(b);
    }
  }
  if (bsize(b) >= size) {
    trim(b, size);
    unlock();
    return p;
  }
  unlock();
  void *q = malloc(n);
  if (q) {
    memcpy(q, p, bsize(b) - HDR);
    free(p);
  }
  return q;
}

void *memalign(size_t align, size_t n) {
  if (align <= HDR)
    return malloc(n);
  if (align & (align - 1) || align > 0x10000000u) {
    errno = EINVAL;
    return NULL;
  }
  int bad = 0;
  u32 size = need(n, &bad);
  if (bad) {
    errno = ENOMEM;
    return NULL;
  }
  lock();
  /* room for a gap of a whole header at least before the aligned payload */
  char *p = alloc(size + (u32)align + HDR);
  if (!p) {
    unlock();
    errno = ENOMEM;
    return NULL;
  }
  struct blk *b = (struct blk *)(p - HDR);
  uintptr_t a = ((uintptr_t)p + align - 1) & ~(uintptr_t)(align - 1);
  if (a != (uintptr_t)p) {
    u32 gap = (u32)(a - (uintptr_t)p); /* a multiple of 16, >= 16 */
    struct blk *nb = (struct blk *)((char *)b + gap);
    nb->prev_size = gap;
    nb->size = (bsize(b) - gap) | USED;
    next_blk(nb)->prev_size = bsize(nb);
    b->size = gap | USED;
    release(b);
    b = nb;
  }
  trim(b, size);
  unlock();
  return (char *)b + HDR;
}

void *aligned_alloc(size_t align, size_t n) { return memalign(align, n); }

int posix_memalign(void **out, size_t align, size_t n) {
  if (align < sizeof(void *) || (align & (align - 1)))
    return EINVAL;
  void *p = memalign(align, n);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

void *valloc(size_t n) { return memalign(4096, n); }
void *pvalloc(size_t n) { return memalign(4096, (n + 4095) & ~(size_t)4095); }

#ifdef MXC_MALLOC_TEST
/* the heap's invariants: sizes and prev_size agree, no two free blocks in
   a row, every free block in the list of its class and only those.
   Returns 0, or a code for the first problem. */
int mxc_malloc_check(void) {
  u32 nfree = 0;
  if (!base)
    return 0;
  for (struct blk *b = (struct blk *)base;; ) {
    if (b->size != (HDR | USED))
      return 1; /* a region's first block */
    struct blk *prev = b;
    b = next_blk(b);
    for (; bsize(b); prev = b, b = next_blk(b)) {
      if (b->prev_size != bsize(prev))
        return 2;
      if (!(b->size & USED)) {
        if (!(prev->size & USED))
          return 3;
        nfree++;
      }
      if (bsize(b) & 15)
        return 4;
    }
    if (b->prev_size != bsize(prev))
      return 5;
    if (b == epilogue)
      break;
    b = (struct blk *)((char *)b + HDR); /* tests use one region */
  }
  u32 listed = 0;
  for (u32 fl = 0; fl < FL_N; fl++)
    for (u32 sl = 0; sl < SL_N; sl++) {
      int bit = !!(sl_map[fl] & 1u << sl);
      if (bit != !!heads[fl][sl])
        return 6;
      u32 prev = 0;
      for (u32 o = heads[fl][sl]; o; prev = o, o = at(o)->next) {
        struct blk *b = at(o);
        u32 f, s;
        mapping(bsize(b), &f, &s);
        if (f != fl || s != sl || (b->size & USED) || b->prev != prev)
          return 7;
        listed++;
      }
    }
  return listed == nfree ? 0 : 8;
}
#endif
