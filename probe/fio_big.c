/* Probe: large file server WRITEs and READs from memory (host:), the sizes
   the swap file of <mx/vm.h> uses: what the server answers, and what the
   file holds. */

#include <mx/mx.h>
#include <mx/sif.h>

static u8 buf[65536] MX_ALIGNED(64), back[65536] MX_ALIGNED(64);

int main(void) {
  int fail = 0;
  for (u32 i = 0; i < sizeof buf; i++)
    buf[i] = (u8)(i * 7 + (i >> 8));
  static const u32 sizes[] = {300, 4096, 16384, 32768, 65536};
  for (u32 k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
    u32 n = sizes[k];
    int fd = mx_fio_open("host:fio_big.bin", MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
    int w = mx_fio_write(fd, buf, n);
    int end = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    mx_fio_close(fd);
    fd = mx_fio_open("host:fio_big.bin", MX_FIO_RDONLY);
    for (u32 i = 0; i < n; i++)
      back[i] = 0;
    int r = mx_fio_read(fd, back, n);
    mx_fio_close(fd);
    u32 bad = 0;
    for (u32 i = 0; i < n; i++)
      bad += back[i] != buf[i];
    mx_printf("fio_big: %u bytes: write %d, size %d, read %d, %u bytes wrong\n", n, w, end, r,
              bad);
    fail |= w != 0 || end != (int)n || r != (int)n || bad;
  }
  mx_printf("fio_big: %s\n", fail ? "FAIL" : "ok");
  MX_TEST_END(fail);
  return fail;
}
