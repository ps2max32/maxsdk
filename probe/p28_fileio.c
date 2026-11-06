/* What rom0:FILEIO gives on each device: the base to compare a new file service
   with. Reads a file from USB and the memory card in reads of 16 and 32 KiB (one
   call each) and writes a temporary file on USB, removed after.

     p28_fileio <file on usb0:> [the IOP modules of USB: mc0:/MXOS]

   Uses the drivers as the shell does: bdm, bdmfs_fatfs, usbd, usbmass_bd from
   mc0:/MXOS. Nothing is written to the memory card. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define KB 1024u
#define MOST (8 * 1024 * KB)
#define WRITE_BYTES (4 * 1024 * KB)
#define WRITE_FILE "usb0:/PS2MAX/p28.tmp"

extern u32 mx__fio_read_chunk;

static u8 buf[256 * KB] MX_ALIGNED(64);
static u8 irx[128 * KB] MX_ALIGNED(64);

static int load(const char *path) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fd;
  int n = mx_fio_read(fd, irx, sizeof irx), res = 0;
  mx_fio_close(fd);
  return n <= 0 ? -1 : mx_iop_load_module_buffer(irx, (u32)n, 0, 0, &res);
}

static void read_file(const char *path, u32 chunk) {
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0) {
    mx_printf("p28: %s: open %d\n", path, fd);
    return;
  }
  u32 total = 0, calls = 0, cycles = 0, max = 0;
  mx__fio_read_chunk = chunk;
  while (total < MOST) {
    u32 t0 = mx_count();
    int got = mx_fio_read(fd, buf, chunk);
    u32 t = mx_count() - t0;
    if (got <= 0)
      break;
    cycles += t, total += (u32)got, calls++;
    if (t > max)
      max = t;
    if ((u32)got < chunk)
      break;
  }
  mx__fio_read_chunk = 0;
  mx_fio_close(fd);
  u32 us = cycles / (MX_EE_HZ / 1000000u);
  mx_printf("p28: read %s, %u KiB a call: %u KiB in %u ms: %u KiB/s, %u us a call, at most %u us\n",
            path, chunk / KB, total / KB, us / 1000, us ? total / KB * 1000u / (us / 1000u + 1) : 0,
            calls ? us / calls : 0, max / (MX_EE_HZ / 1000000u));
}

static void write_file(u32 chunk) {
  int fd = mx_fio_open(WRITE_FILE, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("p28: %s: open %d\n", WRITE_FILE, fd);
    return;
  }
  u32 cycles = 0, total = 0;
  while (total < WRITE_BYTES) {
    u32 t0 = mx_count();
    int r = mx_fio_write(fd, buf, chunk);
    cycles += mx_count() - t0;
    if (r) {
      mx_printf("p28: write failed at %u: %d\n", total, r);
      break;
    }
    total += chunk;
  }
  u32 t0 = mx_count();
  mx_fio_close(fd);
  u32 close_ms = (mx_count() - t0) / (MX_EE_HZ / 1000u), ms = cycles / (MX_EE_HZ / 1000u);
  mx_printf("p28: write usb0:, %u KiB a call: %u KiB in %u ms (close %u ms): %u KiB/s\n", chunk / KB,
            total / KB, ms, close_ms, ms ? total / KB * 1000u / ms : 0);
  mx_fio_remove(WRITE_FILE);
}

int main(int argc, char **argv) {
  if (argc < 2 || mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  static const char *const modules[] = {"mc0:/MXOS/bdm.irx", "mc0:/MXOS/bdmfs_fatfs.irx",
                                        "mc0:/MXOS/usbd.irx", "mc0:/MXOS/usbmass_bd.irx"};
  for (u32 i = 0; i < 4; i++)
    mx_printf("p28: %s: %d\n", modules[i], load(modules[i]));
  int d = -1;
  for (int i = 0; i < 100 && d < 0; i++) {
    d = mx_fio_dopen("usb0:/");
    if (d < 0)
      mx_delay_us(100000);
  }
  if (d < 0) {
    mx_printf("p28: no usb0:\n");
    MX_TEST_END(2);
    return 2;
  }
  mx_fio_dclose(d);
  read_file(argv[1], 16 * KB);
  read_file(argv[1], 32 * KB);
  read_file("mc0:/MXOS/MXSHELL.ELF", 16 * KB);
  read_file("mc0:/MXOS/MXSHELL.ELF", 32 * KB);
  write_file(32 * KB);
  write_file(16 * KB);
  MX_TEST_END(0);
  return 0;
}
