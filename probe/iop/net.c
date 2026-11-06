/* What the IOP's TCP/IP stack can do by itself, without host:, SIF or the EE:
   a host (net_host.py) sends MB MiB to this module, then this module sends as
   much back. A thread of the lowest priority counts while it runs, so the
   counts against an idle second say how much of the IOP was left.

     p25_iop host:net.irx

   Everything on the PS2 is loaded over the network, so a probe of the network
   must not hang it. The stack has no SO_RCVTIMEO: the transfers block, as
   host: does, in a thread of their own, and the start thread gives up at a
   deadline. The module then stays (its thread may still be in the stack) and
   ends in failure, so the EE goes on and the IOP's reset removes it. */

#include "net_imports.h"

IRX_ID("net_probe", 1, 0);

#define PORT 18765
#define MB 8u
#define TICKS_S 36864000u

static u8 buf[32 * 1024] __attribute__((aligned(16)));
static int timer;
static volatile u32 idle_count;
static volatile int idle_stop, idle_done;

#define now() ((u32)GetTimerCounter(timer))

static void idle(void *arg) {
  (void)arg;
  while (!idle_stop)
    idle_count++;
  idle_done = 1;
  ExitDeleteThread();
}

/* wait up to 500 ms: returns > 0 when s is ready */
static int wait_fd(int s, int writing) {
  fd_set set;
  struct timeval tv = {0, 500000};
  FD_ZERO(&set);
  FD_SET(s, &set);
  return select(s + 1, writing ? 0 : &set, writing ? &set : 0, 0, &tv);
}

/* The time in ticks from the first byte on, 0 on failure */
static u32 transfer(int s, u32 total, int sending) {
  u32 t0 = 0, done = 0;
  while (done < total) {
    u32 len = total - done < sizeof buf ? total - done : sizeof buf;
    int n = sending ? send(s, buf, len, 0) : recv(s, buf, len, 0);
    if (n <= 0) {
      printf("net: %d after %u bytes\n", n, done);
      return 0;
    }
    if (!done)
      t0 = now();
    done += (u32)n;
  }
  return now() - t0;
}

static int conn;
static volatile int worker_done;
static u32 rx, rx_span, rx_idle, tx, tx_idle;

static void worker(void *arg) {
  (void)arg;
  u32 bytes = MB * 1024u * 1024u, n0 = idle_count, t0 = now();
  rx = transfer(conn, bytes, 0);
  rx_span = now() - t0, rx_idle = idle_count - n0;
  if (rx) {
    n0 = idle_count, t0 = now();
    /* the host's byte: it has all of it */
    if (transfer(conn, bytes, 1) && transfer(conn, 1, 0))
      tx = now() - t0, tx_idle = idle_count - n0;
  }
  worker_done = 1;
  ExitDeleteThread();
}

/* rate: idle counts per ms */
static void report(const char *what, u32 bytes, u32 ticks, u32 idle_ticks, u32 idle_n, u32 rate) {
  u32 ms = ticks / (TICKS_S / 1000u), idle_ms = idle_ticks / (TICKS_S / 1000u);
  u32 kbs = ms ? bytes / 1024u * 1000u / ms : 0;
  u32 left = idle_ms && rate ? idle_n / idle_ms * 100u / rate : 0;
  printf("net: %s %u KiB in %u ms: %u KiB/s, %u%% of the IOP left\n", what, bytes / 1024u, ms, kbs,
         left);
  DelayThread(2000);
}

static int run(void) {
  iop_thread_t th = {TH_C, 0, idle, 0x800, 126};
  int fail = 1, c = -1, l = socket(AF_INET, SOCK_STREAM, 0);
  int id = CreateThread(&th);
  if (l < 0 || id <= 0) {
    printf("net: socket %d, thread %d\n", l, id);
    if (l >= 0)
      lwip_close(l);
    return 1;
  }
  StartThread(id, 0);

  u32 t0 = now(), n0 = idle_count;
  DelayThread(1000000);
  u32 rate = (idle_count - n0) / ((now() - t0) / (TICKS_S / 1000u));
  printf("net: idle %u counts/ms\n", rate);

  struct sockaddr_in a;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons(PORT);
  if (bind(l, (struct sockaddr *)&a, sizeof a) < 0 || listen(l, 1) < 0) {
    printf("net: cannot listen\n");
    goto end;
  }
  printf("net: listening on %d\n", PORT);
  for (u32 i = 0; i < 30 && c < 0; i++)
    if (wait_fd(l, 0) > 0)
      c = accept(l, 0, 0);
  if (c < 0) {
    printf("net: nobody came\n");
    goto end;
  }

  iop_thread_t wt = {TH_C, 0, worker, 0x1000, 0x40};
  conn = c;
  id = CreateThread(&wt);
  if (id <= 0)
    goto end;
  StartThread(id, 0);
  for (u32 i = 0; i < 400 && !worker_done; i++)
    DelayThread(100000);
  if (!worker_done) {
    printf("net: deadline\n");
    idle_stop = 1;
    return -1;
  }
  u32 bytes = MB * 1024u * 1024u;
  if (rx)
    report("receive", bytes, rx, rx_span, rx_idle, rate);
  if (tx) {
    report("send", bytes, tx, tx, tx_idle, rate);
    fail = 0;
  }
end:
  if (c >= 0)
    lwip_close(c);
  lwip_close(l);
  idle_stop = 1;
  for (u32 i = 0; i < 100 && !idle_done; i++)
    DelayThread(1000);
  return fail;
}

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  timer = AllocHardTimer(1, 32, 1);
  if (timer < 0) {
    printf("net: no timer\n");
    return (1 << 2) | MODULE_NO_RESIDENT_END;
  }
  SetTimerMode(timer, 0);
  int fail = run();
  if (fail < 0)
    return (1 << 2) | MODULE_RESIDENT_END;
  FreeHardTimer(timer);
  return (fail << 2) | MODULE_NO_RESIDENT_END;
}
