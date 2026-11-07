/* What USB mass storage can read without a file system: loads usbd from
   mc0:/MXOS and probe/iop/usbraw.c, which runs its tests while loading and
   prints the results on the IOP's console. Reads only.

     p30_usbraw host:usbraw.irx [mc0:/MXOS/usbd.irx [timing]]

   With "timing" usbraw only times the packets within USB frames.

   usbmass_bd must not be loaded: the device goes to the first class driver. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>

static u8 irx[128 * 1024] MX_ALIGNED(64);

static u32 arg_len(const char *a) {
  u32 n = 0;
  if (!a)
    return 0;
  while (a[n])
    n++;
  return n + 1;
}

static int load(const char *path, const char *arg) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fd;
  int n = mx_fio_read(fd, irx, sizeof irx), res = 0;
  mx_fio_close(fd);
  return n <= 0 ? -1 : mx_iop_load_module_buffer(irx, (u32)n, arg, arg_len(arg), &res);
}

int main(int argc, char **argv) {
  if (argc < 2 || mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  const char *usbd = argc > 2 ? argv[2] : "mc0:/MXOS/usbd.irx";
  int r = load(usbd, 0);
  mx_printf("p30: %s: %d\n", usbd, r);
  r = load(argv[1], argc > 3 ? argv[3] : 0);
  mx_printf("p30: %s: %d\n", argv[1], r);
  MX_TEST_END(r < 0 ? 2 : 0);
  return 0;
}
