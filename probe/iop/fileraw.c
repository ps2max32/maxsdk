/* What the file system driver gives to one read on the IOP, by size of the read:
   the test file of package-games.sh (each 32-bit word its own byte offset), by
   ioman, as mxio's reads do. Reads only. Run by probe/p31_usbfile.c, as the
   last module. */

#include <irx.h>
#include <intrman.h>
#include <loadcore.h>
#include <stdio.h>
#include <thbase.h>
#include <timrman.h>
#include <ioman.h>
#include <sysmem.h>
#include <thsemap.h>

IRX_ID("fileraw_probe", 1, 0);

#define KB 1024u
#define BYTES (2048 * KB)
#define TICKS_MS 36864u

static int done;

static void worker(void *arg) {
  (void)arg;
  static const u32 sizes[] = {16 * KB, 32 * KB, 64 * KB, 128 * KB};
  int timer = AllocHardTimer(1, 32, 1);
  u32 *buf = AllocSysMemory(0, 128 * KB, 0);

  if (timer < 0 || !buf) {
    SignalSema(done);
    ExitDeleteThread();
  }
  SetTimerMode(timer, 0);
  printf("fileraw: IOP memory free %u KiB, largest block %u KiB (buffer of 128 KiB taken)\n",
         QueryTotalFreeMemSize() / KB, QueryMaxFreeMemSize() / KB);
  for (u32 s = 0; s < 4; s++) {
    int fd = open("usb0:/PS2MAX/test/pattern.bin", 1);
    u32 at = 0, wrong = 0, t = 0;
    if (fd < 0) {
      printf("fileraw: open %d\n", fd);
      break;
    }
    while (at < BYTES) {
      u32 t0 = (u32)GetTimerCounter(timer);
      int got = read(fd, buf, (int)sizes[s]);
      t += (u32)GetTimerCounter(timer) - t0;
      if (got != (int)sizes[s]) {
        printf("fileraw: read %d\n", got);
        break;
      }
      for (u32 i = 0; i < sizes[s] / 4; i++)
        wrong += buf[i] != at + i * 4;
      at += sizes[s];
    }
    close(fd);
    u32 ms = t / TICKS_MS + 1;
    printf("fileraw: %u KiB a read: %u KiB/s, wrong words %u\n", sizes[s] / KB, at / KB * 1000u / ms,
           wrong);
  }
  SignalSema(done);
  ExitDeleteThread();
}

/* the file system needs more stack than the thread that starts modules has */
int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  iop_sema_t s = {0, 0, 0, 1};
  iop_thread_t t = {TH_C, 0, worker, 0x4000, 0x40};

  done = CreateSema(&s);
  StartThread(CreateThread(&t), 0);
  WaitSema(done);
  return MODULE_RESIDENT_END;
}
