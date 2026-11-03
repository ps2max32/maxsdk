#include <mx/vram.h>

static u8 owner[MX_VRAM_PAGES]; /* MX_VRAM_FREE is 0 */

void mx_vram_clear(void) {
  for (u32 p = 0; p < MX_VRAM_PAGES; p++)
    owner[p] = MX_VRAM_FREE;
}

u32 mx_vram_find(u32 pages) {
  if (pages == 0 || pages > MX_VRAM_PAGES)
    return ~0u;
  u32 run = 0;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++) {
    run = owner[p] == MX_VRAM_FREE ? run + 1 : 0;
    if (run == pages)
      return p + 1 - pages;
  }
  return ~0u;
}

int mx_vram_take(u32 page, u32 pages, u32 own) {
  if (own == MX_VRAM_FREE || pages == 0 || page >= MX_VRAM_PAGES || pages > MX_VRAM_PAGES - page)
    return -1;
  for (u32 p = page; p < page + pages; p++)
    if (owner[p] != MX_VRAM_FREE)
      return -1;
  for (u32 p = page; p < page + pages; p++)
    owner[p] = (u8)own;
  return 0;
}

void mx_vram_give(u32 page, u32 pages) {
  for (u32 p = page; p < page + pages && p < MX_VRAM_PAGES; p++)
    owner[p] = MX_VRAM_FREE;
}

u32 mx_vram_owner(u32 page) { return owner[page]; }
const u8 *mx__vram_owners(void) { return owner; }

u32 mx_vram_count(u32 own) {
  u32 n = 0;
  for (u32 p = 0; p < MX_VRAM_PAGES; p++)
    n += owner[p] == own;
  return n;
}

u32 mx_vram_end(u32 own) {
  for (u32 p = MX_VRAM_PAGES; p > 0; p--)
    if (owner[p - 1] == own)
      return p;
  return 0;
}
