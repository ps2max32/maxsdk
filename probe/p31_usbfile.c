/* The USB mass storage driver through FILEIO: speed, and whether the data is
   right. Reads the test file package-games.sh puts on the device (each 32-bit
   word its own byte offset), then writes a temporary file, reads it back and
   removes it.

     p31_usbfile <usbmass_bd.irx> [read|all [fileraw.irx]]

   With read it does not write: for a driver that makes faults. A last module
   is loaded when the device is there: probe/iop/fileraw.c. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define KB 1024u
#define PATTERN "usb0:/PS2MAX/test/pattern.bin"
#define TEMP "usb0:/PS2MAX/p31.tmp"
#define BYTES (4 * 1024 * KB)

extern u32 mx__fio_read_chunk;

static u8 buf[128 * KB] MX_ALIGNED(64);
static u8 irx[128 * KB] MX_ALIGNED(64);
static int bad;

static int load(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fd;
  int n = mx_fio_read(fd, irx, sizeof irx), res = 0;
  mx_fio_close(fd);
  return n <= 0 ? -1 : mx_iop_load_module_buffer(irx, (u32)n, 0, 0, &res);
}

static u32 wrong(u32 at, u32 size, u32 key) {
  const u32 *w = (const u32 *)buf;
  u32 n = 0;
  for (u32 i = 0; i < size / 4; i++)
    n += w[i] != ((at + i * 4) ^ key);
  return n;
}

static void read_file(const char *path, u32 chunk, u32 key) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("p31: %s: open %d\n", path, fd);
    bad++;
    return;
  }
  mx_printf("p31: fd %d (mxio from 64)\n", fd);
  u32 total = 0, cycles = 0, errors = 0, calls = 0, max = 0;
  mx__fio_read_chunk = chunk;
  while (total < BYTES) {
    u32 t0 = mx_count();
    int got = mx_fio_read(fd, buf, chunk);
    u32 t = mx_count() - t0;
    if (got <= 0) {
      mx_printf("p31: read at %u: %d\n", total, got);
      bad++;
      break;
    }
    cycles += t, calls++;
    if (t > max)
      max = t;
    errors += wrong(total, (u32)got, key);
    total += (u32)got;
  }
  mx__fio_read_chunk = 0;
  mx_fio_close(fd);
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_printf("p31: read %u KiB a call: %u KiB/s, %u us a call, at most %u, wrong words %u\n", chunk / KB,
            ms ? total / KB * 1000u / ms : 0, calls ? ms * 1000u / calls : 0,
            max / (MX_EE_HZ / 1000000u), errors);
  bad += errors != 0;
}

static void write_file(u32 chunk, u32 key) {
  int fd = mx_fio_open(TEMP, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("p31: %s: open %d\n", TEMP, fd);
    bad++;
    return;
  }
  u32 cycles = 0;
  for (u32 at = 0; at < BYTES; at += chunk) {
    u32 *w = (u32 *)buf;
    for (u32 i = 0; i < chunk / 4; i++)
      w[i] = (at + i * 4) ^ key;
    u32 t0 = mx_count();
    int r = mx_fio_write(fd, buf, chunk);
    cycles += mx_count() - t0;
    if (r) {
      mx_printf("p31: write at %u: %d\n", at, r);
      bad++;
      break;
    }
  }
  mx_fio_close(fd);
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_printf("p31: write %u KiB a call: %u KiB/s\n", chunk / KB, ms ? BYTES / KB * 1000u / ms : 0);
  read_file(TEMP, 32 * KB, key);
  mx_fio_remove(TEMP);
}

int main(int argc, char **argv) {
  if (argc < 2 || mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  static const char *const modules[] = {"mc0:/MXOS/bdm.irx", "mc0:/MXOS/bdmfs_fatfs.irx",
                                        "mc0:/MXOS/usbd.irx"};
  for (u32 i = 0; i < 3; i++)
    mx_printf("p31: %s: %d\n", modules[i], load(modules[i]));
  mx_printf("p31: %s: %d\n", argv[1], load(argv[1]));
  int d = -1;
  for (int i = 0; i < 100 && d < 0; i++) {
    d = mx_fio_dopen("usb0:/");
    if (d < 0)
      mx_delay_us(100000);
  }
  if (d < 0) {
    mx_printf("p31: no usb0:\n");
    MX_TEST_END(2);
    return 2;
  }
  mx_fio_dclose(d);
  if (argc > 3)
    mx_printf("p31: %s: %d\n", argv[3], load(argv[3]));
  read_file(PATTERN, 16 * KB, 0);
  read_file(PATTERN, 32 * KB, 0);
  read_file(PATTERN, 64 * KB, 0);
  if (argc < 3 || argv[2][0] != 'r') {
    write_file(16 * KB, 0x5a5a5a5a);
    write_file(32 * KB, 0xa5a5a5a5);
  }
  MX_TEST_END(bad ? 3 : 0);
  return bad ? 3 : 0;
}
