/* The EE's physical memory (kernel version 21; <mx/proc.h>'s mx_phys_read and
   mx_phys_write): this program (libmxk without VM, its core at a run of its
   own) reads its own data through its physical address and finds what it
   wrote; it may not write (that is for a service); reads outside the
   memory, or into VM, are refused. Started by mxlink:
     k43_phys.elf     (status 0 expected) */
#include "t.h"
#include <mx/cpu.h>
#include <mx/loader.h>
#include <mx/proc.h>
#include <mx/string.h>

MX_CORE_HEAP(256 << 10);

static u8 a[10000] MX_ALIGNED(64), b[10000] MX_ALIGNED(64);

int main(void) {
  for (u32 i = 0; i < sizeof a; i++)
    a[i] = (u8)(i * 7 + 3);
  u32 phys = mx_paddr_of(a).v;
  mx_printf("k43: data at %p, physical %08x (core offset %08x)\n", (void *)a, phys, mx__core_off);
  CHECK_EQ(mx_phys_read(phys, b, sizeof b), 0);
  CHECK_EQ(memcmp(a, b, sizeof a), 0);
  /* a piece from the middle, at an odd address */
  memset(b, 0, sizeof b);
  CHECK_EQ(mx_phys_read(phys + 4097, b + 1, 5000), 0);
  CHECK_EQ(memcmp(a + 4097, b + 1, 5000), 0);
  /* the kernel's memory reads too (its code at 0xC0000) */
  CHECK_EQ(mx_phys_read(0xC0000, b, 64), 0);
  /* refused: writing (not a service), beyond the memory, into VM, none */
  CHECK_EQ(mx_phys_write(phys, b, 16), -1);
  CHECK_EQ(mx_phys_read(0x02000000 - 8, b, 16), -1);
  CHECK_EQ(mx_phys_read(phys, (void *)0x08000000, 16), -1);
  CHECK_EQ(mx_phys_read(phys, b, 0), -1);
  CHECK_EQ(memcmp(a, a, 1), 0);
  return t_done("k43");
}
