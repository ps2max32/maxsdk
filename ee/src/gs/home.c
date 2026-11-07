/* The kernel's home menu (mx-kernel 7, docs/mx-os-plan.md step 1): when the
   kernel asks, the program stops at its next frame boundary
   (mx_frame_begin_n) with the GS idle, lends the kernel GS memory and waits
   until the menu is closed. The memory: free pages, else the texture
   cache's (dropped: the GS reads none of it between frames, and the cache
   fills again). Not in hires mode (mx_frame_sync refuses): the kernel then
   lets the pad's combination end the program. */

#include <mx/frame.h>
#include <mx/gs.h>
#include <mx/kernel.h>
#include <mx/mx.h>
#include <mx/vcache.h>

/* the cache only if the program has it */
extern void mx_vcache_reset(void) __attribute__((weak));

static u32 lend(u32 pages) {
  u32 p = mx_vram_find(pages);
  if (p == ~0u && mx_vcache_reset) {
    mx_vcache_reset();
    p = mx_vram_find(pages);
  }
  if (p == ~0u || mx_vram_take(p, pages, MX_VRAM_KERNEL) != 0)
    return ~0u;
  return p;
}

void mx__home_poll(void) {
  if (!mx__kernel || mx__kernel->version < 7)
    return;
  u32 pages = mx__kernel->home_wanted();
  if (!pages)
    return;
  u32 page = mx_frame_sync() < 0 ? ~0u : lend(pages);
  int end = mx__kernel->home_run(page, pages);
  if (page != ~0u)
    mx_vram_give(page, pages);
  if (end)
    mx_exit(0);
}
