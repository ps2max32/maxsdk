/* The heap: physical memory from the end of the program (_end) to the main
   stack (mx_heap_start .. mx_heap_end), shared by
   - the program break (mx_sbrk, libmxc's malloc), from the bottom up;
   - the core heap (mx_core_alloc), from the top down.
   With VM on, the break moves into VM and the memory between the two
   becomes the frames (vm.c); the core heap keeps what it was given. */

#include <mx/debug.h>
#include <mx/mx.h>
#include <mx/vm.h>

#include "vm_int.h"

/* A block of the core heap, in address order from core_lo up to the top. */
struct blk {
  u32 size; /* bytes, this header included */
  u32 used;
  u32 pad[2];
};

static u8 *brk_lo;     /* the physical break */
static u8 *core_lo;    /* the core heap is [core_lo, top) */
static u8 *core_floor; /* how far down it may grow: the break, or the frames' end */
static u8 *top;

static void heap_setup(void) {
  if (top)
    return;
  brk_lo = (u8 *)(((uintptr_t)mx_heap_start() + 15) & ~(uintptr_t)15);
  top = (u8 *)((uintptr_t)mx_heap_end() & ~(uintptr_t)15);
  core_lo = top;
}

static struct blk *next_blk(struct blk *b) { return (struct blk *)((u8 *)b + b->size); }

/* The user pointer of a block: aligned, with the block's address in the word
   before it. */
static void *user_ptr(struct blk *b, u32 align) {
  uintptr_t p = ((uintptr_t)b + sizeof *b + 4 + align - 1) & ~(uintptr_t)(align - 1);
  ((struct blk **)p)[-1] = b;
  return (void *)p;
}

void *mx_core_alloc(u32 bytes, u32 align) {
  heap_setup();
  if (align < 16)
    align = 16;
  u32 need = ((u32)sizeof(struct blk) + 4 + align - 1 + bytes + 15) & ~15u;
  int was = mx_di();
  /* first fit */
  for (struct blk *b = (struct blk *)core_lo; (u8 *)b < top; b = next_blk(b)) {
    if (b->used || b->size < need)
      continue;
    if (b->size - need >= 64) { /* split: the rest stays free */
      struct blk *r = (struct blk *)((u8 *)b + need);
      r->size = b->size - need;
      r->used = 0;
      b->size = need;
    }
    b->used = 1;
    mx_irq_restore(was);
    return user_ptr(b, align);
  }
  /* grow down */
  u8 *floor = core_floor ? core_floor : brk_lo;
  if ((u32)(core_lo - floor) < need) {
    mx_irq_restore(was);
    return 0;
  }
  core_lo -= need;
  struct blk *b = (struct blk *)core_lo;
  b->size = need;
  b->used = 1;
  mx_irq_restore(was);
  return user_ptr(b, align);
}

void mx_core_free(void *p) {
  if (!p)
    return;
  struct blk *b = ((struct blk **)p)[-1];
  int was = mx_di();
  b->used = 0;
  /* merge free neighbours, then give the lowest free blocks back */
  for (struct blk *a = (struct blk *)core_lo; (u8 *)a < top; a = next_blk(a))
    while (!a->used && (u8 *)next_blk(a) < top && !next_blk(a)->used)
      a->size += next_blk(a)->size;
  struct blk *low = (struct blk *)core_lo;
  if (!low->used && (u8 *)low < top)
    core_lo += low->size;
  mx_irq_restore(was);
}

/* ---- the break ------------------------------------------------------------- */

static u8 *vm_brk;
void *mx__vm_image_end(void);

void *mx_sbrk(s32 incr) {
  heap_setup();
  incr = (incr + 15) & ~15;
  int was = mx_di();
  void *r = (void *)-1;
  if (mx_vm_active()) {
    u8 *lim = (u8 *)mx_vm_base() + mx_vm_size();
    u8 *lo = mx__vm_image_end(); /* after a program linked into VM */
    if (!vm_brk)
      vm_brk = lo;
    if ((incr >= 0 && (u32)(lim - vm_brk) >= (u32)incr) ||
        (incr < 0 && (u32)(vm_brk - lo) >= (u32)-incr)) {
      r = vm_brk;
      vm_brk += incr;
    }
  } else if ((incr >= 0 && (u32)(core_lo - brk_lo) >= (u32)incr) ||
             (incr < 0 && (u32)(brk_lo - (u8 *)mx_heap_start()) >= (u32)-incr)) {
    r = brk_lo;
    brk_lo += incr;
  }
  mx_irq_restore(was);
  return r;
}

/* For vm.c: hand the free middle of the heap (above the break, below the
   core heap less keep bytes) over as frames; the core heap may not grow
   into them. Returns the start (aligned to align), *bytes its size. */
void *mx__heap_take(u32 keep, u32 align, u32 *bytes) {
  heap_setup();
  u8 *lo = (u8 *)(((uintptr_t)brk_lo + align - 1) & ~(uintptr_t)(align - 1));
  u8 *hi = core_lo - keep;
  if ((uintptr_t)core_lo < keep || hi <= lo) {
    *bytes = 0;
    return 0;
  }
  hi = (u8 *)((uintptr_t)hi & ~(uintptr_t)(align - 1));
  *bytes = (u32)(hi - lo);
  core_floor = hi;
  brk_lo = lo + *bytes;
  return lo;
}
