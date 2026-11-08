/* The HDD driver (mxata) through bdm and FILEIO: whether the data is right, and
   the speed. Reads PS2MAX/test/pattern.bin from the disk's FAT partition (each
   32-bit word its own byte offset), then writes a temporary file, reads it
   back and removes it.

     p34_ata [<module dir> [read]]

   The modules (mxdev9, bdm, bdmfs_fatfs, mxata) come from the directory
   (default host:), each unless it runs already. With read it does not write;
   without, it writes the pattern file first (4 MiB) if the disk has none. */

#include <mx/iop.h>
#include <mx/mx.h>
#include <mx/sif.h>
#include <mx/timer.h>

#define KB 1024u
#define PATTERN "ata0:/PS2MAX/test/pattern.bin"
#define TEMP "ata0:/PS2MAX/p34.tmp"
#define BYTES (4 * 1024 * KB)

extern u32 mx__fio_read_chunk;

static u8 buf[128 * KB] MX_ALIGNED(64);
static u8 irx[128 * KB] MX_ALIGNED(64);
static int bad;

static int load(const char *dir, const char *name) {
  struct mx_iop_module m;
  if (mx_iop_find_module(name, &m) > 0)
    return 0;
  char path[128];
  mx_snprintf(path, sizeof path, "%s%s.irx", dir, name);
  int fd = mx_fio_open(path, MX_FIO_RDONLY);
  if (fd < 0)
    return fd;
  int n = mx_fio_read(fd, irx, sizeof irx), res = 0;
  mx_fio_close(fd);
  int id = n <= 0 ? -1 : mx_iop_load_module_buffer(irx, (u32)n, 0, 0, &res);
  mx_printf("p34: %s: %d (%d)\n", path, id, res);
  return id < 0 || res == 1 ? -1 : 0;
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
    mx_printf("p34: %s: open %d\n", path, fd);
    bad++;
    return;
  }
  u32 total = 0, cycles = 0, errors = 0, calls = 0, max = 0;
  mx__fio_read_chunk = chunk;
  while (total < BYTES) {
    u32 t0 = mx_count();
    int got = mx_fio_read(fd, buf, chunk);
    u32 t = mx_count() - t0;
    if (got <= 0) {
      mx_printf("p34: read at %u: %d\n", total, got);
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
  mx_printf("p34: read %u KiB a call: %u KiB/s, %u us a call, at most %u, wrong words %u\n", chunk / KB,
            ms ? total / KB * 1000u / ms : 0, calls ? ms * 1000u / calls : 0,
            max / (MX_EE_HZ / 1000000u), errors);
  bad += errors != 0;
}

static void write_file(const char *path, u32 chunk, u32 key) {
  int fd = mx_fio_open(path, MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd < 0) {
    mx_printf("p34: %s: open %d\n", path, fd);
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
      mx_printf("p34: write at %u: %d\n", at, r);
      bad++;
      break;
    }
  }
  mx_fio_close(fd);
  u32 ms = cycles / (MX_EE_HZ / 1000u);
  mx_printf("p34: write %u KiB a call: %u KiB/s\n", chunk / KB, ms ? BYTES / KB * 1000u / ms : 0);
  read_file(path, 32 * KB, key);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "host:";
  if (mx_sif_init() < 0) {
    MX_TEST_END(1);
    return 1;
  }
  static const char *const modules[] = {"mxdev9", "bdm", "bdmfs_fatfs", "mxata"};
  for (u32 i = 0; i < 4; i++)
    if (load(dir, modules[i]) < 0) {
      MX_TEST_END(1);
      return 1;
    }
  int d = -1;
  for (int i = 0; i < 100 && d < 0; i++) {
    d = mx_fio_dopen("ata0:/");
    if (d < 0)
      mx_delay_us(100000);
  }
  if (d < 0) {
    mx_printf("p34: no ata0:\n");
    MX_TEST_END(2);
    return 2;
  }
  mx_fio_dclose(d);
  char mode = argc > 2 ? argv[2][0] : 0;
  int fd = mx_fio_open(PATTERN, MX_FIO_RDONLY);
  if (fd >= 0)
    mx_fio_close(fd);
  else if (mode != 'r') {
    mx_fio_mkdir("ata0:/PS2MAX", 0777);
    mx_fio_mkdir("ata0:/PS2MAX/test", 0777);
    write_file(PATTERN, 64 * KB, 0);
  }
  read_file(PATTERN, 16 * KB, 0);
  read_file(PATTERN, 32 * KB, 0);
  read_file(PATTERN, 64 * KB, 0);
  if (mode != 'r') {
    /* a new file each time: bdmfs_fatfs fails writes into a FAT32 file that
       O_TRUNC emptied (with ps2sdk's ata_bd too) */
    write_file(TEMP, 16 * KB, 0x5a5a5a5a);
    mx_fio_remove(TEMP);
    write_file(TEMP, 32 * KB, 0xa5a5a5a5);
    mx_fio_remove(TEMP);
  }
  MX_TEST_END(bad ? 3 : 0);
  return bad ? 3 : 0;
}
