/* A file mapped resident (kernel version 36, <mx/vm.h>'s
   mx_vm_map_file_flags with MX_VM_MAP_RESIDENT): the whole file in pinned
   memory, in one physical piece, read without page faults or TLB refills.
   The program writes host:k68.bin (300 KiB of a pattern), maps it resident
   and paged, and checks: both read the pattern; the resident one is at
   MX_PHYS_WIN, has a physical address, took its size out of the pool and
   reading all of it makes no refill of its own; unmapped, the
   frames are back.
     k68_resmap.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/kernel.h>
#include <mx/loader.h>
#include <mx/mem.h>
#include <mx/sif.h>
#include <mx/vm.h>

MX_CORE_HEAP(256 << 10);

#define PAGE MX_VM_PAGE
#define BYTES (300u * 1024)
#define NW (BYTES / 4)

static u32 E(u32 i) { return i * 2654435761u ^ 0xA5A5A5A5u; }
static u32 buf[PAGE / 4] MX_DMA_LINES;
static struct mx_vm_hot hot[256];

int main(void) {
  CHECK(mx__kernel && mx__kernel->version >= 36);
  int fd = mx_fio_open("host:k68.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  for (u32 at = 0; at < BYTES; at += PAGE) {
    u32 n = BYTES - at < PAGE ? BYTES - at : PAGE;
    for (u32 i = 0; i < n / 4; i++)
      buf[i] = E(at / 4 + i);
    CHECK_EQ(mx_fio_write(fd, buf, n), 0);
  }
  mx_fio_close(fd);

  struct mx_vm_stats s0, s1, s2;
  CHECK_EQ(mx_vm_refill_hist(hot, 256), 0); /* the count starts (its table is pinned frames too) */
  mx_vm_stats(&s0);
  u32 size = 0;
  const u32 *r = mx_vm_map_file_flags("host:k68.bin", &size, MX_VM_MAP_RESIDENT);
  mx_printf("k68: resident at %p, %u bytes, physical %08x\n", (const void *)r, size,
            r ? mx_paddr_of(r).v : 0);
  CHECK(r != 0);
  CHECK_EQ(size, BYTES);
  if (!r)
    return t_done("k68");
  CHECK((u32)(uintptr_t)r - MX_PHYS_WIN < MX_PHYS_WIN_LIMIT - MX_PHYS_WIN);
  CHECK_EQ(mx_paddr_of(r).v, (u32)(uintptr_t)r - MX_PHYS_WIN);
  mx_vm_stats(&s1);
  CHECK(s1.pinned_frames - s0.pinned_frames >= BYTES / PAGE);
  (void)mx_vm_refill_hist(hot, 256); /* from 0 */
  u32 bad = 0;
  for (u32 i = 0; i < NW; i++)
    bad += r[i] != E(i);
  CHECK_EQ(bad, 0);
  mx_vm_stats(&s2); /* its faults: of the program's own pages (its code), none of the file's */
  int n = mx_vm_refill_hist(hot, 256);
  u32 in_file = 0;
  for (int i = 0; i < n; i++)
    in_file += hot[i].va - (u32)(uintptr_t)r < BYTES;
  mx_printf("k68: read whole: %u faults, %d pages with refills, %u of them the file's\n",
            s2.faults - s1.faults, n, in_file);
  CHECK_EQ(in_file, 0);

  /* the same file paged, beside it */
  u32 psize = 0;
  const u32 *p = mx_vm_map_file_flags("host:k68.bin", &psize, 0);
  CHECK(p != 0);
  CHECK_EQ(psize, BYTES);
  bad = 0;
  for (u32 i = 0; p && i < NW; i += 97)
    bad += p[i] != E(i);
  CHECK_EQ(bad, 0);
  if (p)
    mx_vm_unmap_file(p);

  mx_vm_unmap_file(r);
  mx_vm_stats(&s2);
  mx_printf("k68: unmapped: %u pinned frames (%u before, %u with the file)\n", s2.pinned_frames,
            s0.pinned_frames, s1.pinned_frames);
  CHECK_EQ(s2.pinned_frames, s0.pinned_frames);
  /* again: the place in the table of such files is free */
  r = mx_vm_map_file_flags("host:k68.bin", &size, MX_VM_MAP_RESIDENT);
  CHECK(r != 0);
  CHECK(r && r[NW - 1] == E(NW - 1));
  return t_done("k68"); /* not unmapped: its frames go with the program */
}
