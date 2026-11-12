/* Heaps by kind (<mx/mem.h>).

   A heap is segments (runs of pinned frames, physical blocks without VM,
   or one range of the heap window for a heap of pages) cut into extents,
   kept in a table sorted by address apart from the memory: an allocation
   is the first free extent it fits in (split off before and after), a free
   merges with its free neighbours in the same segment. A pinned segment
   with nothing used goes back to the pool. */

#include <mx/bios.h>
#include <mx/mem.h>
#include <mx/mx.h>
#include <mx/vm.h>

#include "vm_int.h"

#define MAXHEAPS 16
#define BLOCK (64u << 10)           /* a pinned heap grows by this much, or more */
#define META_BYTES MX_VM_PAGE       /* a heap's first table: 32 KiB */
#define PAGED_DEFAULT (16u << 20)   /* mx_heap_paged's VM */

struct ext {
  u32 a, n;  /* address, bytes */
  u16 seg;   /* its segment */
  u8 used;
  u8 pad;
};

struct seg {
  u32 a, n;
  u8 frames; /* frames of the pool (mx__vm_frames_take), else physical blocks */
  u8 va;     /* a range of the heap window: never given back */
  u8 live;
};

struct mx_heap {
  const char *name;
  int kind;
  u32 max;
  int paged; /* a heap of pages in the heap window (VM on) */
  int ready;
  void *meta; /* its tables: segs, then exts */
  u32 meta_bytes;
  struct seg *seg;
  u32 nseg, scap;
  struct ext *ext;
  u32 next, ecap;
  u32 used, held;
};

static struct mx_heap heaps[MAXHEAPS] = {
    {.name = "pinned", .kind = MX_MEM_PINNED},
    {.name = "stream", .kind = MX_MEM_STREAM},
    {.name = "paged", .kind = MX_MEM_PAGED, .max = PAGED_DEFAULT},
};
static u32 nheaps = 3;
struct mx_heap *const mx_heap_pinned = &heaps[0], *const mx_heap_stream = &heaps[1],
                      *const mx_heap_paged = &heaps[2];

static int sema = -1;
static void lock(void) {
  if (sema < 0) {
    struct {
      s32 count, max_count, init_count, wait_threads;
      u32 attr, option;
    } sp = {0, 1, 1, 0, 0, 0};
    sema = mx_bios_create_sema(&sp);
  }
  if (sema >= 0)
    mx_bios_wait_sema(sema);
}
static void unlock(void) {
  if (sema >= 0)
    mx_bios_signal_sema(sema);
}

static int is_pinned(int kind) { return kind == MX_MEM_PINNED || kind == MX_MEM_STREAM; }

/* memory for tables: pinned (frames with VM, physical without) */
static void *meta_alloc(u32 bytes) {
  return mx_vm_active() ? mx__vm_frames_take(bytes) : mx__phys_alloc(bytes, 64);
}
static void meta_free(void *p, u32 bytes) {
  if (mx_vm_active())
    mx__vm_frames_give(p, bytes);
  else
    mx__phys_free(p);
}

/* the tables with room for one segment and two extents more */
static int room(struct mx_heap *h) {
  if (h->meta && h->nseg < h->scap && h->next + 2 <= h->ecap)
    return 0;
  u32 bytes = h->meta ? h->meta_bytes * 2 : META_BYTES;
  u8 *m = meta_alloc(bytes);
  if (!m)
    return -1;
  u32 scap = bytes / 8 / sizeof(struct seg); /* an eighth for segments */
  struct seg *s = (struct seg *)m;
  struct ext *e = (struct ext *)(m + scap * sizeof(struct seg));
  u32 ecap = (bytes - scap * (u32)sizeof(struct seg)) / sizeof(struct ext);
  for (u32 i = 0; i < h->nseg; i++)
    s[i] = h->seg[i];
  for (u32 i = 0; i < h->next; i++)
    e[i] = h->ext[i];
  if (h->meta)
    meta_free(h->meta, h->meta_bytes);
  h->meta = m, h->meta_bytes = bytes;
  h->seg = s, h->scap = scap;
  h->ext = e, h->ecap = ecap;
  return 0;
}

static void ext_insert(struct mx_heap *h, u32 i, struct ext e) {
  for (u32 j = h->next; j > i; j--)
    h->ext[j] = h->ext[j - 1];
  h->ext[i] = e;
  h->next++;
}
static void ext_remove(struct mx_heap *h, u32 i) {
  for (u32 j = i; j + 1 < h->next; j++)
    h->ext[j] = h->ext[j + 1];
  h->next--;
}

/* a new segment of [a, a + n), all free */
static void seg_add(struct mx_heap *h, u32 a, u32 n, int frames, int va) {
  u32 si = 0;
  while (si < h->nseg && h->seg[si].live)
    si++;
  if (si == h->nseg)
    h->nseg++;
  h->seg[si] = (struct seg){a, n, (u8)frames, (u8)va, 1};
  u32 i = 0;
  while (i < h->next && h->ext[i].a < a)
    i++;
  ext_insert(h, i, (struct ext){a, n, (u16)si, 0, 0});
  if (!va)
    h->held += n;
}

static void setup(struct mx_heap *h) {
  if (h->ready)
    return;
  h->ready = 1;
  h->paged = !is_pinned(h->kind) && mx_vm_active();
  if (h->paged && h->max && !room(h)) {
    u32 va = mx__vm_va_take(h->max);
    if (va)
      seg_add(h, va, (h->max + MX_VM_PAGE - 1) & ~(MX_VM_PAGE - 1), 0, 1);
  }
}

struct mx_heap *mx_heap_create(const char *name, int kind, u32 max_bytes) {
  lock();
  struct mx_heap *h = 0;
  if (nheaps < MAXHEAPS) {
    h = &heaps[nheaps++];
    *h = (struct mx_heap){.name = name, .kind = kind, .max = max_bytes};
    setup(h);
    if (h->paged && !h->nseg) {
      mx_printf("mx_heap_create %s: no room in the heap window for %u KB (mx_vm_config.heap_size)\n",
                name, max_bytes / 1024);
      nheaps--;
      h = 0;
    }
  }
  unlock();
  return h;
}

/* the first free extent that holds bytes at align: split, marked used */
static void *fit(struct mx_heap *h, u32 bytes, u32 align) {
  for (u32 i = 0; i < h->next; i++) {
    struct ext e = h->ext[i];
    if (e.used)
      continue;
    u32 a = (e.a + align - 1) & ~(align - 1);
    if (a < e.a || a - e.a + bytes > e.n)
      continue;
    if (h->next + 2 > h->ecap)
      return 0; /* the caller makes room and asks again */
    u32 before = a - e.a, after = e.n - before - bytes;
    h->ext[i] = (struct ext){a, bytes, e.seg, 1, 0};
    if (after)
      ext_insert(h, i + 1, (struct ext){a + bytes, after, e.seg, 0, 0});
    if (before)
      ext_insert(h, i, (struct ext){e.a, before, e.seg, 0, 0});
    h->used += bytes;
    return (void *)(uintptr_t)a;
  }
  return 0;
}

void *mx_heap_alloc(struct mx_heap *h, u32 bytes, u32 align) {
  if (!h || !bytes)
    return 0;
  if (align < 16)
    align = 16;
  bytes = (bytes + 15) & ~15u;
  lock();
  setup(h);
  void *p = 0;
  if (room(h) == 0)
    p = fit(h, bytes, align);
  if (!p && !h->paged && (!h->max || h->held + bytes <= h->max) && room(h) == 0) {
    /* grow: a block, or a run of its own (frames are 32 KiB aligned) */
    u32 n = bytes + (align > MX_VM_PAGE ? align : 0);
    n = n < BLOCK ? BLOCK : (n + MX_VM_PAGE - 1) & ~(MX_VM_PAGE - 1);
    int frames = mx_vm_active();
    void *m = frames ? mx__vm_frames_take(n) : mx__phys_alloc(n, align > 64 ? align : 64);
    if (m) {
      seg_add(h, (u32)(uintptr_t)m, n, frames, 0);
      p = fit(h, bytes, align);
    }
  }
  unlock();
  return p;
}

/* extent i (free) is its whole segment: give the segment back */
static void seg_release(struct mx_heap *h, u32 i) {
  struct seg *s = &h->seg[h->ext[i].seg];
  if (s->va)
    return;
  if (s->frames)
    mx__vm_frames_give((void *)(uintptr_t)s->a, s->n);
  else
    mx__phys_free((void *)(uintptr_t)s->a);
  h->held -= s->n;
  s->live = 0;
  ext_remove(h, i);
}

static void free_ext(struct mx_heap *h, u32 i) {
  h->ext[i].used = 0;
  h->used -= h->ext[i].n;
  if (h->paged)
    mx__vm_discard((const void *)(uintptr_t)h->ext[i].a, h->ext[i].n);
  if (i + 1 < h->next && !h->ext[i + 1].used && h->ext[i + 1].seg == h->ext[i].seg &&
      h->ext[i].a + h->ext[i].n == h->ext[i + 1].a) {
    h->ext[i].n += h->ext[i + 1].n;
    ext_remove(h, i + 1);
  }
  if (i && !h->ext[i - 1].used && h->ext[i - 1].seg == h->ext[i].seg &&
      h->ext[i - 1].a + h->ext[i - 1].n == h->ext[i].a) {
    h->ext[i - 1].n += h->ext[i].n;
    ext_remove(h, i);
    i--;
  }
  struct seg *s = &h->seg[h->ext[i].seg];
  if (h->ext[i].a == s->a && h->ext[i].n == s->n)
    seg_release(h, i);
}

void mx_heap_free(struct mx_heap *h, void *p) {
  if (!h || !p)
    return;
  u32 a = (u32)(uintptr_t)p;
  lock();
  u32 lo = 0, hi = h->next;
  while (lo < hi) { /* the extent at a */
    u32 mid = (lo + hi) / 2;
    if (h->ext[mid].a < a)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo < h->next && h->ext[lo].a == a && h->ext[lo].used)
    free_ext(h, lo);
  else
    mx_printf("mx_heap_free %s: %p was not allocated there\n", h->name, p);
  unlock();
}

void mx_heap_reset(struct mx_heap *h) {
  if (!h)
    return;
  lock();
  for (u32 i = 0; i < h->next;) {
    if (h->ext[i].used) {
      u32 n = h->next;
      free_ext(h, i);
      if (h->next < n)
        i = 0; /* merged or released: from the start again */
      continue;
    }
    i++;
  }
  unlock();
}

void mx_heap_stats(const struct mx_heap *h, u32 *used, u32 *held) {
  u32 u = h ? h->used : 0, n = h ? h->held : 0;
  if (h && h->paged) { /* its pages resident */
    n = 0;
    for (u32 i = 0; i < h->nseg; i++)
      if (h->seg[i].live)
        n += mx__vm_resident_bytes((const void *)(uintptr_t)h->seg[i].a, h->seg[i].n);
  }
  if (used)
    *used = u;
  if (held)
    *held = n;
}

/* ---- addresses, assets, pressure -------------------------------------------------- */

u32 mx_mem_run(const void *p, u32 bytes) {
  if (!mx_is_vm(p))
    return bytes;
  return mx_vm_first_piece(p, bytes).bytes;
}

int mx_mem_resident(const void *p, u32 bytes) { return mx__vm_resident(p, bytes); }

void mx_mem_prefetch(const void *p, u32 bytes) { mx__vm_prefetch(p, bytes); }

void mx_mem_cold(const void *p, u32 bytes) { mx__vm_cold(p, bytes); }

int mx_mem_lock_try(const void *p, u32 bytes) {
  if (!mx_is_vm(p))
    return 0;
  if (!mx__vm_resident(p, bytes)) {
    mx__vm_prefetch(p, bytes);
    return -1;
  }
  return mx_vm_lock(p, bytes);
}

void mx_mem_pressure(struct mx_mem_pressure *p) {
  static struct mx_mem_pressure last;
  static u32 t0, ins0, outs0, refills0;
  struct mx_vm_stats s;
  *p = (struct mx_mem_pressure){0};
  if (!mx_vm_active())
    return;
  mx_vm_stats(&s);
  u32 now = mx_count(), dt = now - t0;
  if (t0 && dt < MX_EE_HZ / 4) {
    p->swap_ins_s = last.swap_ins_s, p->swap_outs_s = last.swap_outs_s, p->refills_s = last.refills_s;
  } else {
    u32 ms = dt / (MX_EE_HZ / 1000);
    if (t0 && ms) {
      p->swap_ins_s = (s.swap_ins - ins0) * 1000 / ms;
      p->swap_outs_s = (s.swap_outs + s.writer_outs - outs0) * 1000 / ms;
      p->refills_s = (u32)((u64)(s.refills - refills0) * 1000 / ms);
    }
    t0 = now, ins0 = s.swap_ins, outs0 = s.swap_outs + s.writer_outs, refills0 = s.refills;
    last = *p;
  }
  p->frames = s.frames;
  p->free_frames = s.free_frames;
  p->pinned_frames = s.pinned_frames;
}
