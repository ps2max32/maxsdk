/* Textures whose upper levels may be paged out (<mx/vcache.h>,
   struct mx_tex_mem): split memory, and placing from the finest level that
   is resident. Apart from vcache.c, which the host tests build alone. */

#include <mx/frame.h>
#include <mx/mem.h>
#include <mx/tex.h>
#include <mx/vcache.h>
#include <mx/vm.h>

u32 mx__vcache_now(void);
u32 mx__vcache_inits(void);
/* the head is locked for the frame being built (and that frame's lock is
   not one of a pipeline stopped since: its numbers start again) */
static int locked_now(const struct mx_tex_mem *m) {
  return m->lock_frame == mx__vcache_now() + 1 && m->lock_init == (u8)mx__vcache_inits();
}
static void lock_note(struct mx_tex_mem *m) {
  if (mx_is_vm(m->head))
    mx_frame_unlock(m->head, m->head_bytes);
  m->lock_frame = mx__vcache_now() + 1;
  m->lock_init = (u8)mx__vcache_inits();
}
void mx__vcache_count(int upgrade);

u32 mx_tex_level_bytes(u32 psm, u32 tw, u32 th, u32 lv) {
  u32 bits = psm == MX_GS_PSM_T4 ? 4 : psm == MX_GS_PSM_T8 ? 8
           : psm == MX_GS_PSM_CT16 || psm == MX_GS_PSM_CT16S ? 16 : 32;
  u32 w = 1u << (tw > lv ? tw - lv : 0), h = 1u << (th > lv ? th - lv : 0);
  return (w * h * bits / 8 + 63) & ~63u;
}

int mx_tex_mem_alloc(struct mx_tex_mem *m, u32 psm, u32 tw, u32 th, u32 levels, u32 clut_bytes,
                     struct mx_heap *pinned, struct mx_heap *asset) {
  *m = (struct mx_tex_mem){0};
  m->levels = (u8)levels;
  /* the tail: 2 levels down, not below MX_VCACHE_TAIL_MIN a side */
  u32 tail = 0;
  if (asset)
    while (tail < 2 && tail + 1 < levels && (1u << tw) >> (tail + 1) >= MX_VCACHE_TAIL_MIN &&
           (1u << th) >> (tail + 1) >= MX_VCACHE_TAIL_MIN)
      tail++;
  m->tail = (u8)tail;
  for (u32 lv = 0; lv < levels; lv++)
    if (lv < tail)
      m->head_bytes += mx_tex_level_bytes(psm, tw, th, lv);
    else
      m->tail_bytes += mx_tex_level_bytes(psm, tw, th, lv);
  u32 cb = (clut_bytes + 63) & ~63u;
  m->tail_mem = mx_heap_alloc(pinned, m->tail_bytes + cb, 64);
  if (!m->tail_mem)
    return -1;
  if (tail) {
    m->head = mx_heap_alloc(asset, m->head_bytes, 64);
    if (!m->head) {
      mx_heap_free(pinned, m->tail_mem);
      *m = (struct mx_tex_mem){0};
      return -1;
    }
  }
  u8 *h = m->head, *t = m->tail_mem;
  for (u32 lv = 0; lv < levels; lv++) {
    u32 n = mx_tex_level_bytes(psm, tw, th, lv);
    if (lv < tail)
      m->level[lv] = h, h += n;
    else
      m->level[lv] = t, t += n;
  }
  if (clut_bytes)
    m->clut = t;
  return 0;
}

void mx_tex_mem_free(struct mx_tex_mem *m, struct mx_heap *pinned, struct mx_heap *asset) {
  if (m->head)
    mx_heap_free(asset, m->head);
  if (m->tail_mem)
    mx_heap_free(pinned, m->tail_mem);
  *m = (struct mx_tex_mem){0};
}

int mx_vcache_tex_mem(u64 key, struct mx_tex_mem *m, u32 psm, u32 tw, u32 th, u32 cpsm, u32 need,
                      int wait, struct mx_tex *t, u32 *from) {
  u32 f = need < m->levels ? need : m->levels - 1u;
  /* the levels above the tail: in GS memory already, or the head locked */
  if (f < m->tail) {
    struct mx_tex in;
    int there = mx_vcache_peek(mx_vcache_tex_key(key, f), &in);
    int locked = locked_now(m);
    if (!there && !locked) {
      /* no tail to draw meanwhile (one level left: glTexSubImage2D): wait */
      wait |= m->levels <= m->tail;
      locked = wait ? mx_vm_lock(m->head, m->head_bytes) == 0
                    : mx_mem_lock_try(m->head, m->head_bytes) == 0;
      if (locked)
        lock_note(m);
    }
    if (!there && !locked) {
      mx__vcache_count(0);
      f = m->tail; /* the pager brings the head in meanwhile */
    } else if (!there && m->from >= m->tail && m->from > f)
      mx__vcache_count(1);
  }
  m->from = (u8)f;
  *from = f;
  return mx_vcache_tex(mx_vcache_tex_key(key, f), psm, tw - f, th - f, m->levels - f, cpsm, t);
}

void mx_tex_mem_lock(struct mx_tex_mem *m) {
  if (!m->head || locked_now(m))
    return;
  if (mx_vm_lock(m->head, m->head_bytes) == 0)
    lock_note(m);
}
