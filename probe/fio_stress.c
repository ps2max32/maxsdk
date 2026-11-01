/* Probe: the swap file's traffic without the VM: a host: file opened read-
   write, then 4000 seeks + 32 KiB writes or reads in a random order, each
   read checked against what was written there. Does the file server (here
   ps2link's host:) keep up, or stop answering? Progress every 250. */

#include <mx/mx.h>
#include <mx/sif.h>

#define SLOT 32768u
#define NSLOT 192
#define N 4000

static u32 buf[SLOT / 4] MX_ALIGNED(64);
static u32 gen[NSLOT]; /* what each slot holds (0: never written) */

static u32 word(u32 slot, u32 g, u32 i) { return (slot * 2654435761u) ^ (g * 40503u) ^ i; }

/* fio_stress.elf at: 32 KiB reads into fixed addresses of the heap
   (0x1c0000 .. 0x400000 in 32 KiB steps): does the destination matter? */
static int reads_at(int fd) {
  for (u32 i = 0; i < SLOT / 4; i++)
    buf[i] = word(0, 1, i);
  mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
  mx_fio_write(fd, buf, SLOT);
  int bad = 0;
  for (u32 a = 0x1c0000; a < 0x400000 && !bad; a += SLOT) {
    if (a < (u32)(uintptr_t)mx_heap_start())
      continue;
    mx_printf("fio_stress: read into %08x\n", a);
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    int r = mx_fio_read(fd, (void *)(uintptr_t)a, SLOT);
    const u32 *d = (const u32 *)(uintptr_t)a;
    for (u32 i = 0; i < SLOT / 4; i++)
      bad |= d[i] != word(0, 1, i);
    bad |= r != (int)SLOT;
  }
  mx_fio_close(fd);
  mx_printf("fio_stress: reads at addresses %s\n", bad ? "FAIL" : "ok");
  MX_TEST_END(bad);
  return bad;
}

static volatile u32 ticks;
static void tick(void *arg) {
  (void)arg;
  ticks++;
}

/* fio_stress.elf gs: the same with the GS set up (vsync interrupts) and a
   100 ms timer, as tests/ps2/vm.c runs; gsonly, timer: one of them */
int main(int argc, char **argv) {
  /* gs: both; gsonly; timer */
  if (argc >= 2 && argv[1][0] == 'g')
    mx_gs_init(640, 448);
  if (argc >= 2 && (argv[1][0] == 't' || (argv[1][0] == 'g' && argv[1][2] != 'o')))
    mx_timer_periodic(1, 57600, tick, 0);
  int fd = mx_fio_open("host:fio_stress.bin", 3 | MX_FIO_CREAT | MX_FIO_TRUNC);
  if (fd >= 0 && argc >= 2 && argv[1][0] == 'a')
    return reads_at(fd);
  if (fd < 0) {
    mx_printf("fio_stress: open %d\n", fd);
    MX_TEST_END(1);
    return 1;
  }
  u32 x = 1, bad = 0, nw = 0, nr = 0, t0 = mx_count();
  u32 written = 0; /* slots written in order first: the file grows at its end */
  for (u32 k = 0; k < N && !bad; k++) {
    x = x * 1103515245u + 12345u;
    u32 slot = written < NSLOT && (x >> 28) < 6 ? written : (x >> 8) % (written ? written : 1);
    int write = slot == written || (x >> 20 & 1);
    if (mx_fio_lseek(fd, (s32)(slot * SLOT), MX_FIO_SEEK_SET) != (int)(slot * SLOT)) {
      mx_printf("fio_stress: seek failed at %u\n", k);
      bad = 1;
      break;
    }
    if (write) {
      u32 g = ++gen[slot];
      for (u32 i = 0; i < SLOT / 4; i++)
        buf[i] = word(slot, g, i);
      if (mx_fio_write(fd, buf, SLOT) != 0) {
        mx_printf("fio_stress: write failed at %u\n", k);
        bad = 1;
      }
      if (slot == written)
        written++;
      nw++;
    } else {
      int r = mx_fio_read(fd, buf, SLOT);
      u32 wrong = 0;
      for (u32 i = 0; i < SLOT / 4; i++)
        wrong += buf[i] != word(slot, gen[slot], i);
      if (r != (int)SLOT || wrong) {
        mx_printf("fio_stress: read at %u: %d bytes, %u words wrong\n", k, r, wrong);
        bad = 1;
      }
      nr++;
    }
    if ((k + 1) % 250 == 0)
      mx_printf("fio_stress: %u (%u writes, %u reads), %u ms\n", k + 1, nw, nr,
                (mx_count() - t0) / (MX_EE_HZ / 1000));
  }
  mx_fio_close(fd);
  mx_printf("fio_stress: %s\n", bad ? "FAIL" : "ok");
  MX_TEST_END(bad);
  return (int)bad;
}
