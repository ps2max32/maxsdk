/* Pads and Bluetooth dongles on USB (step 0 of docs/pad-bt-plan.md): loads
   usbd from mc0:/MXOS (if it is not running yet) and probe/iop/hidprobe.c,
   which runs for the given seconds and prints on the IOP's console.

     p35_hid host:hidprobe.irx [seconds [usbd path]]

   Loading the modules makes mxlink reset the IOP when the program ends. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>

static u8 irx[64 * 1024] MX_ALIGNED(64);

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
  if (n <= 0)
    return -1;
  int r = mx_iop_load_module_buffer(irx, (u32)n, arg, arg_len(arg), &res);
  return r < 0 ? r : res;
}

int main(int argc, char **argv) {
  if (argc < 2 || mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  const char *secs = argc > 2 ? argv[2] : "30";
  const char *usbd = argc > 3 ? argv[3] : "mc0:/MXOS/usbd.irx";
  int r = load(usbd, 0);
  mx_printf("p35: %s: %d (an error is fine when usbd already runs)\n", usbd, r);
  r = load(argv[1], secs);
  mx_printf("p35: %s: %d\n", argv[1], r);
  MX_TEST_END(r < 0 ? 2 : 0);
  return 0;
}
