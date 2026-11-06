/* SIF waits that sleep (ee/src/base/sif.c, sleep_wait): while a thread waits
   for the IOP, threads of lower priority run. The main thread does 100
   reads of host:sif_sleep.bin (32 KiB each) and 100 writes; a busy thread
   of lower priority counts meanwhile. With waits that spin it would not
   run at all. Also: a call that gives up (an RPC to a server that answers
   late: not tested here) and the waits of a thread with interrupts off
   (they spin) are left to fio.c and the VM tests. */

#include "t.h"
#include <mx/sif.h>
#include <mx/thread.h>

#define KB 1024u

static u8 buf[32 * KB] MX_ALIGNED(64);
static u8 tstack[16 * KB] MX_ALIGNED(16);
static volatile u32 count;
static volatile int stop;

static void busy(void *arg) {
  (void)arg;
  while (!stop)
    count++;
}

int main(void) {
  int fd = mx_fio_open("host:sif_sleep.bin", 3 /* RDWR */ | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("sif_sleep");
  for (u32 i = 0; i < sizeof buf; i++)
    buf[i] = (u8)(i * 7);
  CHECK_EQ(mx_fio_write(fd, buf, sizeof buf), 0);

  mx_thread_set_prio(mx_thread_self(), 30);
  int t = mx_thread_start(busy, 0, tstack, sizeof tstack, 40); /* lower: runs when main waits */
  CHECK(t >= 0);

  u32 c0 = count, t0 = mx_count(), bad = 0;
  for (int n = 0; n < 100; n++) {
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    bad += mx_fio_read(fd, buf, sizeof buf) != (int)sizeof buf;
    mx_fio_lseek(fd, 0, MX_FIO_SEEK_SET);
    bad += mx_fio_write(fd, buf, sizeof buf) != 0;
  }
  u32 ms = (mx_count() - t0) / (MX_EE_HZ / 1000), c1 = count;
  stop = 1;
  if (t >= 0)
    mx_thread_join(t);
  mx_fio_close(fd);

  mx_printf("sif_sleep: 200 transfers in %u ms; the busy thread counted %u meanwhile\n", ms,
            c1 - c0);
  CHECK_EQ(bad, 0);
  CHECK(c1 - c0 > 1000);
  for (u32 i = 0; i < sizeof buf; i++)
    bad += buf[i] != (u8)(i * 7);
  CHECK_EQ(bad, 0);
  return t_done("sif_sleep");
}
