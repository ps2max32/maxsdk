/* The console in place of udptty (docs/iop-drivers.md): the IOP's "tty"
   device (stdin, stdout, stderr: the IOP's printf, the EE's through the tty)
   and Kprintf.

   What is written goes into a ring. A thread sends it on:
   - to the host whose HELLO names a session, as TTY frames of the fast path
     (mxfast.h), without lwIP: the host answers each with how far it has the
     stream, and what it has not got is sent again after RESEND_US. While the
     host is there, a writer waits for room in the ring rather than lose
     output. A host silent for HOST_GONE_US is gone; a new session (a new
     ps2client) starts at what comes next, so that nothing of the last run's
     end reaches it. Each frame names this start of the module (instance): after
     an IOP reset the host takes the new stream from its first frame.
   - else, as udptty did, by UDP broadcast to port 18194 through lwIP's
     socket (looked up when needed: the console works without lwIP too), for
     any host. At the start this waits HOLD_US for a host to say HELLO, so the
     IOP's start-up messages reach it. */
#include "irx_imports.h"
#include <mxfast.h>
#include "mxfast_int.h"

#define RING 16384u
#define FRAMES 4 /* TTY frames on their way */
#define PIECE MXFAST_CHUNK
#define RESEND_US 100000
#define HOST_GONE_US 3000000
#define HOLD_US 2000000
#define BCAST_PORT 18194
#define BCAST_PIECE 1024
#define DRIVER_PRIO 0x28 /* mxeth's */
#define PRIO 0x30 /* below the driver's, above mxio's (0x50) */

static u8 ring[RING];
/* offsets in the stream: written, sent, and taken by the host (or given up) */
static volatile u32 w, sent, acked;
static volatile u32 session; /* the host's, 0: none */
static volatile u32 hello_at, ack_at; /* system clock, low word */
static int con_wake = -1, con_space = -1, space_waiters;
static u32 start_at, instance;
static volatile int holding = 1;

static u32 now(void) {
  iop_sys_clock_t c;
  GetSystemTime(&c);
  return c.lo;
}
static u32 us_ticks(u32 us) {
  iop_sys_clock_t c;
  USec2SysClock(us, &c);
  return c.lo;
}

static void wake(void) {
  if (QueryIntrContext())
    iSignalSema(con_wake);
  else
    SignalSema(con_wake);
}

/* ---- writing */

/* A thread that may wait for the host: one below the driver's, whose rx
   brings the answers that make room */
static int may_wait_here(void) {
  iop_thread_info_t i;
  return ReferThreadStatus(TH_SELF, &i) >= 0 && i.currentPriority > DRIVER_PRIO;
}

/* n bytes into the ring. may_wait: a thread; it waits for room while a host
   takes the console. Else the oldest bytes give way. */
static void put(const u8 *b, u32 n, int may_wait) {
  int st;
  if (n > RING)
    b += n - RING, n = RING;
  CpuSuspendIntr(&st);
  while (RING - (w - acked) < n) {
    if (may_wait && session && may_wait_here()) {
      space_waiters++;
      CpuResumeIntr(st);
      WaitSema(con_space);
      CpuSuspendIntr(&st);
      continue;
    }
    acked = w - (RING - n);
    if ((int)(sent - acked) < 0)
      sent = acked;
  }
  u32 at = w % RING, first = RING - at < n ? RING - at : n;
  memcpy(ring + at, b, first);
  memcpy(ring, b + first, n - first);
  w += n;
  CpuResumeIntr(st);
  wake();
}

/* the thread: room again */
static void freed(void) {
  int st, n;
  CpuSuspendIntr(&st);
  n = space_waiters;
  space_waiters = 0;
  CpuResumeIntr(st);
  while (n--)
    SignalSema(con_space);
}

/* ---- from rx, in the driver thread */

void console_hello(u32 s) {
  hello_at = now();
  if (s == session)
    return;
  int st;
  CpuSuspendIntr(&st);
  if (session || !holding)
    sent = acked = w; /* the last session's rest, or what was broadcast */
  session = s;
  holding = 0;
  ack_at = hello_at;
  CpuResumeIntr(st);
  wake();
}

void console_ack(u32 have) {
  int st;
  CpuSuspendIntr(&st);
  if (session && (int)(have - acked) > 0 && (int)(have - sent) <= 0) {
    acked = have;
    ack_at = now();
  }
  CpuResumeIntr(st);
  wake();
}

/* ---- to the host: TTY frames */

static struct {
  struct mxeth_tx tx;
  volatile int busy;
  u8 f[42 + MXFAST_HDR + PIECE] __attribute__((aligned(16)));
} frame[FRAMES];

static void frame_done(struct mxeth_tx *t) {
  for (int i = 0; i < FRAMES; i++)
    if (&frame[i].tx == t)
      frame[i].busy = 0;
  wake();
}

/* What is not sent yet, while frames are free */
static void send_host(void) {
  for (int i = 0; i < FRAMES && sent != w; i++) {
    if (frame[i].busy)
      continue;
    u32 at = sent, n = w - at;
    if (n > PIECE)
      n = PIECE;
    u8 *f = frame[i].f;
    fast_headers(f, MXFAST_HDR + n);
    fast_header(f + 42, MXFAST_TTY, 0, at, instance, 0);
    f[54] = n, f[55] = n >> 8;
    u32 r = at % RING, first = RING - r < n ? RING - r : n;
    memcpy(f + 42 + MXFAST_HDR, ring + r, first);
    memcpy(f + 42 + MXFAST_HDR + first, ring, n - first);
    frame[i].busy = 1;
    frame[i].tx.data = f;
    frame[i].tx.len = 42 + MXFAST_HDR + n;
    frame[i].tx.done = frame_done;
    sent = at + n;
    mxeth_send(&frame[i].tx);
  }
}

/* ---- to anyone: udptty's broadcast, through lwIP */

struct lwip_sockaddr_in {
  u8 len, family;
  u16 port;
  u32 addr;
  u8 zero[8];
};
typedef int lwip_socket_fn(int domain, int type, int protocol);
typedef int lwip_sendto_fn(int s, const void *data, u32 size, int flags,
                           const struct lwip_sockaddr_in *to, u32 tolen);

static void send_broadcast(void) {
  static iop_library_t lib;
  static void **ps2ip;
  static int sock = -1;
  static u8 piece[BCAST_PIECE];
  if (!ps2ip) {
    memset(&lib, 0, sizeof lib);
    lib.version = 0x206;
    memcpy(lib.name, "ps2ip\0\0", 8);
    ps2ip = QueryLibraryEntryTable(&lib);
  }
  if (ps2ip && sock < 0)
    sock = ((lwip_socket_fn *)ps2ip[13])(2, 2, 17); /* AF_INET, SOCK_DGRAM, UDP */
  while (sent != w) {
    u32 at = sent, n = w - at;
    if (n > BCAST_PIECE)
      n = BCAST_PIECE;
    u32 r = at % RING, first = RING - r < n ? RING - r : n;
    memcpy(piece, ring + r, first);
    memcpy(piece + first, ring, n - first);
    sent = acked = at + n;
    if (sock >= 0) {
      struct lwip_sockaddr_in to;
      memset(&to, 0, sizeof to);
      to.len = sizeof to, to.family = 2;
      to.port = (u16)(BCAST_PORT >> 8 | (BCAST_PORT & 255) << 8);
      to.addr = 0xFFFFFFFF;
      ((lwip_sendto_fn *)ps2ip[12])(sock, piece, n, 0, &to, sizeof to);
    }
  }
  freed();
}

/* ---- the thread */

static unsigned int alarm_rang(void *arg) {
  (void)arg;
  iSignalSema(con_wake);
  return 0;
}

static void thread(void *arg) {
  (void)arg;
  for (;;) {
    u32 t = now(), wait = 0;
    if (session && t - hello_at > us_ticks(HOST_GONE_US)) {
      /* gone: what it did not take is dropped (it may have stopped at it) */
      int st;
      CpuSuspendIntr(&st);
      session = 0;
      sent = acked = w;
      CpuResumeIntr(st);
      freed();
    }
    if (session) {
      if (sent != acked && t - ack_at > us_ticks(RESEND_US)) {
        sent = acked; /* from where the host is */
        ack_at = t;
      }
      send_host();
      freed();
      if (sent != acked || sent != w)
        wait = RESEND_US;
    } else if (holding && t - start_at < us_ticks(HOLD_US)) {
      wait = HOLD_US / 10;
    } else {
      holding = 0;
      send_broadcast();
    }
    if (wait) {
      iop_sys_clock_t c;
      USec2SysClock(wait, &c);
      SetAlarm(&c, alarm_rang, 0);
      WaitSema(con_wake);
      CancelAlarm(alarm_rang, 0);
    } else
      WaitSema(con_wake);
  }
}

/* ---- the tty device, as udptty's */

static int tty_nop(void) { return 0; }
static int tty_eio(void) { return -5; }
static int tty_write(iop_file_t *file, void *buf, int size) {
  (void)file;
  if (size > 0)
    put(buf, (u32)size, !QueryIntrContext());
  return size;
}

static iop_device_ops_t tty_ops = {
    (void *)tty_nop, (void *)tty_nop, (void *)tty_eio, (void *)tty_nop, (void *)tty_nop,
    (void *)tty_eio, (void *)tty_write, (void *)tty_eio, (void *)tty_eio, (void *)tty_eio,
    (void *)tty_eio, (void *)tty_eio, (void *)tty_eio, (void *)tty_eio, (void *)tty_eio,
    (void *)tty_eio, (void *)tty_eio,
};
static iop_device_t tty_device = {"tty", IOP_DT_CHAR | IOP_DT_CONS, 1, "TTY via mxfast", &tty_ops};

/* Kprintf: formatted in kernel mode, maybe in an interrupt */
struct kpr {
  u32 n;
  char b[256];
};
static void kpr_char(void *ctx, int c) {
  struct kpr *k = ctx;
  if (c > 0 && c < 256 && k->n < sizeof k->b)
    k->b[k->n++] = (char)c;
}
static int kpr_format(void *ctx, const char *format, void *ap) {
  if (format)
    prnt(kpr_char, ctx, format, ap);
  return 0;
}
static int kpr_handler(void *context, const char *format, va_list ap) {
  struct kpr k;
  (void)context;
  k.n = 0;
  int r = CpuInvokeInKmode(kpr_format, &k, format, ap);
  put((const u8 *)k.b, k.n, 0);
  return r;
}

int console_start(void) {
  iop_sema_t s = {0, 0, 0, 1};
  iop_sema_t sp = {0, 0, 0, 64};
  iop_thread_t th = {TH_C, 0, thread, 0x800, PRIO};
  int tid;
  iop_sys_clock_t c;
  GetSystemTime(&c);
  start_at = c.lo;
  instance = (c.lo ^ c.hi << 16) | 1; /* this start's stream, for the host */
  if ((con_wake = CreateSema(&s)) < 0 || (con_space = CreateSema(&sp)) < 0 ||
      (tid = CreateThread(&th)) < 0 || StartThread(tid, 0) < 0)
    return -1;
  DelDrv(tty_device.name);
  if (AddDrv(&tty_device) < 0)
    return -1;
  close(0);
  open("tty00:", 0x1000 | O_RDWR);
  close(1);
  open("tty00:", O_WRONLY);
  close(2);
  open("tty00:", O_WRONLY);
  KprintfSet(kpr_handler, 0);
  return 0;
}
