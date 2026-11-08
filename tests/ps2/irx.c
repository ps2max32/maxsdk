/* IOP modules from ld.lld --irx, loaded both ways the IOP can: by path
   (LOADFILE reads the file itself, seeking to the headers, the segment, the
   section headers and the relocations) and from EE memory (LoadModuleBuffer).

     irx.elf host:<module>.irx...

   Each module must stay loadable twice (a module without a resident end, as
   probe/iop/r3000a.irx, or one that only prints). */

#include <mx/iop.h>
#include <mx/sif.h>
#include "t.h"

static u8 irx[256 * 1024] MX_ALIGNED(64);

int main(int argc, char **argv) {
  CHECK(mx_sif_init() == 0);
  CHECK(argc > 1);
  for (int i = 1; i < argc; i++) {
    int id = mx_iop_load_module(argv[i]);
    mx_printf("irx: %s by path: id %d\n", argv[i], id);
    CHECK(id >= 0);

    int fd = mx_fio_open(argv[i], MX_FIO_RDONLY);
    CHECK(fd >= 0);
    if (fd < 0)
      continue;
    int size = mx_fio_lseek(fd, 0, MX_FIO_SEEK_END);
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    CHECK(size > 0 && size <= (int)sizeof irx);
    if (size > 0 && size <= (int)sizeof irx)
      CHECK_EQ(mx_fio_read(fd, irx, (u32)size), size);
    mx_fio_close(fd);
    int res = -99;
    id = mx_iop_load_module_buffer(irx, (u32)size, 0, 0, &res);
    mx_printf("irx: %s from EE memory (%d bytes): id %d, start result %d\n", argv[i], size, id,
              res);
    CHECK(id >= 0);
  }
  return t_done("irx");
}
