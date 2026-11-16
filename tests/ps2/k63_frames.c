/* Every frame of the VM holds what was written to it (mx OS plan step 4a,
   part 3: the BIOS's memory below 1 MiB is frames too, on a console where
   the kernel does not use the BIOS). The program takes all free frames but
   a few as VM (mx_sbrk), writes a pattern of its own to every word, reads it
   back, and the same again with another pattern; then it looks in the
   physical memory below 1 MiB (mx_phys_read) for its pattern, to say how
   many of those frames it had. Without a swap file the pages stay where they
   are; with one, some go out and come back, which is checked just the same.
     k63_frames.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/vm.h>
#include <mx/mx.h>

MX_CORE_HEAP(256 << 10);

#define PAGE 32768u
#define KEEP 8u /* frames left free: this program's own code and tables */
#define TAG 0xA5000000u

static u32 word(u32 page, u32 i, u32 round) { return TAG | ((page * 2654435761u + i * 40503u + round * 977u) & 0xFFFFFFu); }

static u32 buf[PAGE / 4] MX_DMA_LINES;

int main(void) {
  struct mx_vm_stats s;
  mx_vm_stats(&s);
  mx_printf("k63: %u frames, %u free, %u pinned frames\n", s.frames, s.free_frames, s.pinned_frames);
  CHECK(s.free_frames > 64);
  u32 n = s.free_frames - KEEP;
  /* page aligned: a page of VM a frame */
  u8 *brk = mx_sbrk((s32)(n * PAGE + PAGE));
  CHECK(brk != 0 && brk != (u8 *)-1);
  if (!brk || brk == (u8 *)-1)
    return t_done("k63");
  u32 *m = (u32 *)(((uintptr_t)brk + PAGE - 1) & ~(uintptr_t)(PAGE - 1));
  u32 bad = 0;
  for (u32 round = 0; round < 2; round++) {
    for (u32 p = 0; p < n; p++)
      for (u32 i = 0; i < PAGE / 4; i++)
        m[p * (PAGE / 4) + i] = word(p, i, round);
    for (u32 p = 0; p < n; p++)
      for (u32 i = 0; i < PAGE / 4; i++)
        if (m[p * (PAGE / 4) + i] != word(p, i, round) && bad++ < 8)
          mx_printf("k63: round %u page %u word %u: %08x, not %08x\n", round, p, i,
                    m[p * (PAGE / 4) + i], word(p, i, round));
  }
  CHECK_EQ(bad, 0);
  /* which frames below 1 MiB hold a page of it: all 8192 words tagged */
  u32 low = 0;
  for (u32 a = PAGE; a < 0x00100000u; a += PAGE) {
    if (mx_phys_read(a, buf, sizeof buf) < 0)
      continue;
    u32 tagged = 0;
    for (u32 i = 0; i < PAGE / 4; i++)
      tagged += (buf[i] & 0xFF000000u) == TAG;
    low += tagged == PAGE / 4;
  }
  mx_vm_stats(&s);
  mx_printf("k63: %u pages (%u MiB) written and read twice; %u frames below 1 MiB held one; %u swap outs\n", n,
            n * PAGE >> 20, low, s.swap_outs);
  if (s.frames > 960)
    CHECK(low > 0);
  return t_done("k63");
}
