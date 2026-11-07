#ifndef MX_VRAM_H
#define MX_VRAM_H

/* GS memory by pages: who owns each of its 512 pages of 8 KiB (2048
   words, 32 blocks of 64 words). One map for all of it: render targets
   (mx_vram_alloc, <mx/gs.h>), the cache (<mx/vcache.h>) and what an
   application takes itself. Pure bookkeeping: no GS access. */

#include <mx/types.h>

#define MX_VRAM_PAGES 512
#define MX_VRAM_BLOCKS (MX_VRAM_PAGES * 32)

/* owners of a page */
/* MX_VRAM_KERNEL: lent to the kernel (its home menu: ee/src/gs/home.c) */
enum { MX_VRAM_FREE = 0, MX_VRAM_FIXED = 1, MX_VRAM_CACHE = 2, MX_VRAM_APP = 3, MX_VRAM_KERNEL = 4 };

/* Every page free (mx_vram_reset does this and more). */
void mx_vram_clear(void);
/* The first run of pages free pages (first fit from page 0), or ~0u. */
u32 mx_vram_find(u32 pages);
/* Take pages page .. page + pages - 1 for owner (not MX_VRAM_FREE): 0, or
   -1 if one of them is not free or out of range (then none is taken). */
int mx_vram_take(u32 page, u32 pages, u32 owner);
/* Make the pages free again, whoever owns them. */
void mx_vram_give(u32 page, u32 pages);
/* The owner of page (< MX_VRAM_PAGES). */
u32 mx_vram_owner(u32 page);
/* The pages owner has. */
u32 mx_vram_count(u32 owner);
/* One past the highest page owner has, 0 if none. */
u32 mx_vram_end(u32 owner);

#endif
