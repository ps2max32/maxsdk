/* File reads into buffers at every offset in a cache line and of sizes from
   1 byte to 64 KiB, the bytes around them written just before (dirty in the
   cache): the read must not lose them (the IOP's DMA and the cache lines at
   the buffer's ends). A program that takes all memory. Started by the kernel:
     k44_read.elf     (status 0 expected) */
#include "t.h"
#include <mx/sif.h>
#include <mx/string.h>

/* core: the IOP's DMA writes it (the program is linked into VM) */
static u8 area[3 * 65536 + 256] MX_DMA_LINES;
static u8 ref[65536 + 64];

int main(void) {
  int fd = mx_fio_open("host:k44_read.elf", 0);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("k44");
  int n = mx_fio_read(fd, ref, sizeof ref);
  mx_printf("k44: ref %d bytes\n", n);
  static const u32 sizes[] = {1, 3, 16, 63, 64, 65, 100, 511, 512, 1000, 4096, 5000, 16384, 40000, 65536};
  u32 bad = 0, runs = 0;
  for (u32 si = 0; si < sizeof sizes / sizeof sizes[0]; si++)
    for (u32 off = 0; off < 64; off += 1) {
      u32 sz = sizes[si];
      u8 *dst = area + 128 + off;
      /* the neighbours dirty in the cache */
      for (u32 i = 0; i < 128 + 64; i++)
        area[i] = (u8)(0xA0 + i);
      memset(dst, 0xEE, sz);
      for (u32 i = 0; i < 128; i++)
        dst[sz + i] = (u8)(0x50 + i);
      mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
      int r = mx_fio_read(fd, dst, sz);
      runs++;
      int ok = r == (int)sz && !memcmp(dst, ref, sz);
      for (u32 i = 0; i < 128 + off; i++)
        if (area[i] != (u8)(0xA0 + i))
          ok = 0;
      for (u32 i = 0; i < 128; i++)
        if (dst[sz + i] != (u8)(0x50 + i))
          ok = 0;
      if (!ok && bad++ < 10)
        mx_printf("k44: size %u off %u: bad (read %d)\n", sz, off, r);
    }
  mx_fio_close(fd);
  mx_printf("k44: %u of %u reads bad\n", bad, runs);
  CHECK(bad == 0);
  return t_done("k44");
}
