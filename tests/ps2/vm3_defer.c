/* VM step 3: host: I/O asked for from an interrupt handler and done in a
   thread: a timer interrupt (every 100 ms) signals a semaphore, a worker
   thread of higher priority wakes, seeks, writes 32 KiB and reads it back,
   while the main thread spins (as a game's would). The interrupt itself
   only signals: file I/O waits for the IOP's answer, which an interrupt
   handler cannot. About 10 s; at the end the screen is bright green with
   900D0000 in slot 0, or bright red with BAD000nn.

   Slot 0 and the colour: the step (1 running, 2 checked). Slot 1: the
   main thread's spin count (moves while it runs), slot 2: requests from
   the interrupt, slot 3: rounds the worker finished, slot 4: where the
   worker is (1 woken, 2 sought, 3 written, 4 read, 5 checked). Heartbeat
   in slots 30 and 31. */

#include "t.h"
#include <mx/sif.h>

#define SLOT 32768u
#define ROUNDS 60

static u8 buf[SLOT] MX_ALIGNED(64), back[SLOT] MX_ALIGNED(64);
static u8 wstack[16384] MX_ALIGNED(16);
static int fd, sema;
static volatile u32 asked, done, bad, stop;

static u8 byte(u32 seed, u32 i) { return (u8)((i * 2654435761u ^ seed * 40503u) >> 13); }

static void ask(void *arg) {
  (void)arg;
  if (asked < ROUNDS) {
    asked++;
    mx_isema_signal(sema);
  }
}

static void worker(void *arg) {
  (void)arg;
  for (;;) {
    mx_sema_wait(sema);
    if (stop)
      return;
    u32 k = done;
    mx_dbg_word(4, 1);
    u32 at = (k & 7) * SLOT;
    for (u32 i = 0; i < SLOT; i++)
      buf[i] = byte(k, i);
    int ok = mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET) == (int)at;
    mx_dbg_word(4, 2);
    ok = ok && mx_fio_write(fd, buf, SLOT) == 0;
    mx_dbg_word(4, 3);
    ok = ok && mx_fio_lseek(fd, (s32)at, MX_FIO_SEEK_SET) == (int)at &&
         mx_fio_read(fd, back, SLOT) == (int)SLOT;
    mx_dbg_word(4, 4);
    for (u32 i = 0; ok && i < SLOT; i++)
      ok = back[i] == byte(k, i);
    mx_dbg_word(4, 5);
    if (!ok) {
      bad++;
      mx_printf("vm3: round %u failed\n", k);
    }
    done = k + 1;
    mx_dbg_word(3, done);
  }
}

int main(void) {
  mx_dbg_screen();
  mx_dbg_heartbeat(1);
  mx_dbg_step(1);
  fd = mx_fio_open("host:vm3.bin", MX_FIO_RDONLY | MX_FIO_WRONLY | MX_FIO_CREAT | MX_FIO_TRUNC);
  CHECK(fd >= 0);
  if (fd < 0)
    return t_done("vm3");
  sema = mx_sema_create(0, ROUNDS + 1);
  /* the main thread starts at 0, the highest */
  mx_thread_set_prio(mx_thread_self(), 64);
  int w = mx_thread_start(worker, 0, wstack, sizeof wstack, 63);
  CHECK(w >= 0);
  mx_timer_periodic(1, 57600, ask, 0); /* 100 ms */
  u32 spins = 0, v0 = mx_gs.vsync;
  while (done < ROUNDS && mx_gs.vsync - v0 < 30 * mx_gs.hz) { /* 30 s at most */
    if (!(++spins & 0xFFFF)) {
      mx_dbg_word(1, spins);
      mx_dbg_word(2, asked);
    }
  }
  mx_timer_periodic(1, 0, 0, 0);
  mx_dbg_step(2);
  mx_printf("vm3: %u asked, %u done, %u bad, %u spins\n", asked, done, bad, spins);
  CHECK_EQ(done, ROUNDS);
  CHECK_EQ(bad, 0);
  stop = 1;
  mx_sema_signal(sema);
  mx_thread_join(w);
  mx_fio_close(fd);
  mx_dbg_heartbeat(0);
  return t_done("vm3");
}
