/* The region (kernel version 42, <mx/cpu.h>'s MX_VM_REGION_BASE): mapped
   files and heaps of pages get a free piece of it, and only the pages
   touched cost memory. Before, a file window of 64 MiB and a heap window of
   64 MiB, at most 8 files, and a window's space came back only at its end.
   The program writes host:k80big.bin (100 MiB, written at three places:
   the rest is a hole) and host:k80s.bin (40 KiB), and checks:
   - the big file maps whole and reads right at its start, at 70 MiB (past
     the old window) and in its last page;
   - 12 mappings of the small file beside it (more than 8), each in a piece
     of its own, every one read right;
   - one of them unmapped and mapped again is at the same place, and a file
     too large for that hole goes elsewhere;
   - a heap of pages of 100 MiB: a piece of its own, written and read at
     both ends;
   - everything unmapped: the VM holds no more pinned frames than before.
     k80_region.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/sif.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define PIECE (2u << 20)
#define BIG (100u << 20)
#define MID (70u << 20)
#define SMALL (40u * 1024)
#define NSMALL 12

static u32 E(u32 i) { return i * 2654435761u ^ 0x5A5A5A5Au; }
static u32 buf[PAGE / 4] MX_DMA_LINES;

/* a page of the pattern for the words from byte at on */
static int put(int fd, u32 at, u32 bytes) {
  for (u32 i = 0; i < bytes / 4; i++)
    buf[i] = E(at / 4 + i);
  if (mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET) != (int)at)
    return -1;
  return mx_fio_write(fd, buf, bytes);
}

static u32 wrong(const u32 *m, u32 at, u32 bytes) {
  u32 bad = 0;
  for (u32 i = at / 4; i < (at + bytes) / 4; i++)
    bad += m[i] != E(i);
  return bad;
}

static int in_region(const void *p, u32 bytes) {
  u32 a = (u32)(uintptr_t)p;
  return a >= MX_VM_REGION_BASE && a + bytes <= MX_VM_REGION_LIMIT && a + bytes > a;
}

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 42);
  int fd = mx_fio_open("host:k80big.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  CHECK_EQ(put(fd, 0, PAGE), 0);
  CHECK_EQ(put(fd, MID, PAGE), 0);
  CHECK_EQ(put(fd, BIG - PAGE, PAGE), 0);
  mx_fio_close(fd);
  fd = mx_fio_open("host:k80s.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  CHECK_EQ(put(fd, 0, PAGE), 0);
  CHECK_EQ(put(fd, PAGE, SMALL - PAGE), 0);
  mx_fio_close(fd);

  struct mx_vm_stats s0, s1;
  mx_vm_stats(&s0);
  u32 size = 0;
  const u32 *big = mx_vm_map_file("host:k80big.bin", &size);
  mx_printf("k80: 100 MiB mapped at %p, %u bytes\n", (const void *)big, size);
  CHECK(big != 0);
  CHECK_EQ(size, BIG);
  if (!big)
    return t_done("k80");
  CHECK(in_region(big, BIG));
  CHECK_EQ((u32)(uintptr_t)big % PIECE, 0);
  CHECK_EQ(wrong(big, 0, PAGE), 0);
  CHECK_EQ(wrong(big, MID, PAGE), 0);
  CHECK_EQ(wrong(big, BIG - PAGE, PAGE), 0);
  CHECK_EQ(big[(MID - PAGE) / 4], 0); /* the hole */

  const u32 *sm[NSMALL];
  for (int i = 0; i < NSMALL; i++) {
    u32 sz = 0;
    sm[i] = mx_vm_map_file("host:k80s.bin", &sz);
    CHECK(sm[i] != 0);
    CHECK_EQ(sz, SMALL);
    if (!sm[i])
      return t_done("k80");
    CHECK(in_region(sm[i], SMALL));
    /* not in the big file, not where an earlier one is */
    CHECK((u32)((const u8 *)sm[i] - (const u8 *)big) >= BIG);
    for (int k = 0; k < i; k++)
      CHECK((u32)((const u8 *)sm[i] - (const u8 *)sm[k]) >= PIECE &&
            (u32)((const u8 *)sm[k] - (const u8 *)sm[i]) >= PIECE);
    CHECK_EQ(wrong(sm[i], 0, SMALL), 0);
  }
  mx_printf("k80: %d small files, the first at %p, the last at %p\n", NSMALL, (const void *)sm[0],
            (const void *)sm[NSMALL - 1]);
  /* a hole of one piece: the small file fits again, the big one does not */
  const u32 *was = sm[5];
  mx_vm_unmap_file(sm[5]);
  const u32 *big2 = mx_vm_map_file("host:k80big.bin", 0);
  CHECK(big2 != 0);
  CHECK(big2 && (u32)((const u8 *)big2 - (const u8 *)was) >= PIECE);
  CHECK(big2 && (u32)((const u8 *)big2 - (const u8 *)big) >= BIG &&
        (u32)((const u8 *)big - (const u8 *)big2) >= BIG);
  CHECK(big2 && wrong(big2, MID, PAGE) == 0);
  sm[5] = mx_vm_map_file("host:k80s.bin", 0);
  CHECK_EQ((uintptr_t)sm[5], (uintptr_t)was);
  CHECK(sm[5] && wrong(sm[5], 0, SMALL) == 0);
  /* the others are as they were */
  CHECK_EQ(wrong(sm[4], 0, SMALL), 0);
  CHECK_EQ(wrong(sm[6], 0, SMALL), 0);
  CHECK_EQ(wrong(big, BIG - PAGE, PAGE), 0);

  /* a heap of pages: a piece of its own; what is not touched costs nothing */
  struct mx_heap *h = mx_heap_create("k80", MX_MEM_PAGED, 100u << 20);
  CHECK(h != 0);
  u32 *a = h ? mx_heap_alloc(h, 1u << 20, 64) : 0;
  u32 *skip = h ? mx_heap_alloc(h, 90u << 20, 64) : 0; /* never touched */
  u32 *b = h ? mx_heap_alloc(h, 1u << 20, 64) : 0;
  mx_printf("k80: a heap of 100 MiB: %p, %p (90 MiB, not touched), %p\n", (void *)a, (void *)skip,
            (void *)b);
  CHECK(a && skip && b);
  if (a && b) {
    CHECK(in_region(a, 1u << 20) && in_region(b, 1u << 20));
    CHECK((u32)((u8 *)a - (const u8 *)big) >= BIG && (u32)((u8 *)b - (const u8 *)big) >= BIG);
    CHECK((u32)((u8 *)a - (const u8 *)big2) >= BIG && (u32)((u8 *)b - (const u8 *)big2) >= BIG);
    for (u32 i = 0; i < (1u << 20) / 4; i += 1024)
      a[i] = E(i), b[i] = ~E(i);
    u32 bad = 0;
    for (u32 i = 0; i < (1u << 20) / 4; i += 1024)
      bad += a[i] != E(i) || b[i] != ~E(i);
    CHECK_EQ(bad, 0);
    CHECK_EQ(wrong(sm[0], 0, SMALL), 0);
  }

  for (int i = 0; i < NSMALL; i++)
    mx_vm_unmap_file(sm[i]);
  mx_vm_unmap_file(big);
  if (big2)
    mx_vm_unmap_file(big2);
  if (h)
    mx_heap_reset(h);
  /* all of the region's files gone: mapped again, the first place again */
  const u32 *again = mx_vm_map_file("host:k80s.bin", 0);
  CHECK_EQ((uintptr_t)again, (uintptr_t)big);
  CHECK(again && wrong(again, 0, SMALL) == 0);
  if (again)
    mx_vm_unmap_file(again);
  mx_vm_stats(&s1);
  mx_printf("k80: %u pinned frames, %u before; %u faults\n", s1.pinned_frames, s0.pinned_frames,
            s1.faults - s0.faults);
  CHECK(s1.pinned_frames <= s0.pinned_frames + 1); /* the leaves of what stays: a frame at most */
  return t_done("k80");
}
