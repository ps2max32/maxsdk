/* Loads an IOP module given as host:<file> and waits for it to end: for the
   probes in probe/iop/, which print to the console themselves. The rest of the
   arguments go to the module. A module reports a failure in its start result
   (bits 2 and up, as probe/iop/r3000a.c does). */

#include <mx/iop.h>
#include <mx/sif.h>
#include "../tests/ps2/t.h"

static u8 irx[256 * 1024] MX_ALIGNED(64);

int main(int argc, char **argv) {
  CHECK(mx_sif_init() == 0);
  CHECK(argc >= 2);
  if (argc < 2)
    return t_done("p25_iop");

  int fd = mx_fio_open(argv[1], MX_FIO_RDONLY);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("p25_iop");
  int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
  CHECK(size > 0 && size <= (int)sizeof irx);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  CHECK_EQ(mx_fio_read(fd, irx, (u32)size), size);
  mx_fio_close(fd);

  char args[128];
  u32 len = 0;
  for (int i = 2; i < argc; i++)
    for (const char *p = argv[i]; len < sizeof args - 1; p++) {
      args[len++] = *p;
      if (!*p)
        break;
    }

  int res = -1;
  int id = mx_iop_load_module_buffer(irx, (u32)size, len ? args : 0, len, &res);
  mx_printf("p25_iop: %s: id %d, start result %d\n", argv[1], id, res);
  CHECK(id >= 0);
  CHECK_EQ(res >> 2, 0);
  return t_done("p25_iop");
}
