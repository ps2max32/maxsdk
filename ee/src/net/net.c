/* Sockets: the EE's side of common/include/mxsock.h (<mx/net.h>). */
#include <mx/cpu.h>
#include <mx/net.h>
#include <mx/sif.h>
#include <mx/string.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include "../../../common/include/mxsock.h"

static struct mx_sif_client srv;
static u32 in[MXSOCK_ARGS + MXSOCK_MAX / 4] MX_DMA_LINES;
static s32 out[MXSOCK_ANSWER + MXSOCK_MAX / 4] MX_DMA_LINES;
/* one call at a time through srv, in and out: + 1, 0 not made yet */
static int lock;

int mx_net_init(void) {
  if (!lock) {
    int s = mx_sema_create(1, 1), was = mx_di();
    if (!lock && s >= 0)
      lock = s + 1, s = -1;
    mx_irq_restore(was);
    if (s >= 0)
      mx_sema_delete(s);
  }
  if (mx_sif_bound(&srv))
    return 0;
  if (mx_sif_init() < 0 || mx_sif_bind_tries(&srv, MXSOCK_SID, 100) < 0 || !srv.server)
    return -1;
  return 0;
}

/* The call: the data already in in (under the lock), wanted back in out; the
   answer's data is taken by take before the lock goes. */
static int call_data(int fno, u32 a0, u32 a1, u32 a2, u32 a3, const void *data, u32 send,
                     u32 back, void *to, u32 *who) {
  if (mx_net_init() < 0 || !lock)
    return -1;
  mx_sema_wait(lock - 1);
  if (send)
    memcpy(in + MXSOCK_ARGS, data, send);
  in[0] = a0, in[1] = a1, in[2] = a2, in[3] = a3;
  mx_dcache_writeback(in, sizeof in);
  volatile s32 *r = MX_UNCACHED(out);
  r[0] = -1;
  u32 ssize = (MXSOCK_ARGS * 4 + send + 15) & ~15u;
  u32 rsize = (MXSOCK_ANSWER * 4 + back + 15) & ~15u;
  int n = mx_sif_call(&srv, fno, in, ssize, (void *)r, rsize) < 0 ? -1 : r[0];
  if (n > 0 && to)
    memcpy(to, (const void *)MX_UNCACHED(out + MXSOCK_ANSWER), (u32)n);
  if (who)
    who[0] = (u32)r[1], who[1] = (u32)r[2];
  mx_sema_signal(lock - 1);
  return n;
}

static int call(int fno, u32 a0, u32 a1, u32 a2, u32 a3, u32 send, u32 back) {
  (void)send, (void)back;
  return call_data(fno, a0, a1, a2, a3, 0, 0, 0, 0, 0);
}

int mx_net_socket(int type) { return call(MXSOCK_SOCKET, (u32)type, 0, 0, 0, 0, 0); }
int mx_net_bind(int s, u32 port) { return call(MXSOCK_BIND, (u32)s, port, 0, 0, 0, 0); }
int mx_net_listen(int s, int backlog) { return call(MXSOCK_LISTEN, (u32)s, (u32)backlog, 0, 0, 0, 0); }
int mx_net_connect(int s, u32 ip, u32 port) { return call(MXSOCK_CONNECT, (u32)s, ip, port, 0, 0, 0); }
int mx_net_close(int s) { return call(MXSOCK_CLOSE, (u32)s, 0, 0, 0, 0, 0); }
int mx_net_poll(int s, int want) { return call(MXSOCK_POLL, (u32)s, (u32)want, 0, 0, 0, 0); }

int mx_net_accept(int s, u32 *ip, u32 *port) {
  u32 who[2];
  int n = call_data(MXSOCK_ACCEPT, (u32)s, 0, 0, 0, 0, 0, 0, 0, who);
  if (n >= 0 && ip)
    *ip = who[0];
  if (n >= 0 && port)
    *port = who[1];
  return n;
}

static u32 limit(u32 size) { return size > MXSOCK_MAX ? MXSOCK_MAX : size; }

int mx_net_send(int s, const void *data, u32 size) {
  size = limit(size);
  return call_data(MXSOCK_SEND, (u32)s, size, 0, 0, data, size, 0, 0, 0);
}

int mx_net_sendto(int s, const void *data, u32 size, u32 ip, u32 port) {
  size = limit(size);
  return call_data(MXSOCK_SENDTO, (u32)s, size, ip, port, data, size, 0, 0, 0);
}

int mx_net_recv(int s, void *data, u32 size) {
  size = limit(size);
  return call_data(MXSOCK_RECV, (u32)s, size, 0, 0, 0, 0, size, data, 0);
}

int mx_net_recvfrom(int s, void *data, u32 size, u32 *ip, u32 *port) {
  u32 who[2];
  size = limit(size);
  int n = call_data(MXSOCK_RECVFROM, (u32)s, size, 0, 0, 0, 0, size, data, who);
  if (n >= 0 && ip)
    *ip = who[0];
  if (n >= 0 && port)
    *port = who[1];
  return n;
}

/* Waiting: the wait services (mxsock.h), each with its own buffers, so that
   several threads sleep at once while the other calls go on. */
static struct mx_sif_client waiter[MXSOCK_WAITERS];
static u32 wait_buf[MXSOCK_WAITERS][16] MX_DMA_LINES; /* 64 bytes: in, then the answer */
static u32 waiter_busy, waiter_none; /* bits: in use; not offered by the IOP */

static int waiter_take(void) {
  int was = mx_di();
  for (int i = 0; i < MXSOCK_WAITERS; i++)
    if (!((waiter_busy | waiter_none) >> i & 1)) {
      waiter_busy |= 1u << i;
      mx_irq_restore(was);
      return i;
    }
  mx_irq_restore(was);
  return -1;
}

static void waiter_give(int i) {
  int was = mx_di();
  waiter_busy &= ~(1u << i);
  mx_irq_restore(was);
}

/* One wait at the IOP, at most MXSOCK_WAIT_MAX_US: ready count, or < 0 when there
   is no wait service to ask */
static int wait_once(int i, u32 rd, u32 wr, u32 us, u32 *rr, u32 *rw) {
  if (!mx_sif_bound(&waiter[i]) &&
      (mx_sif_bind_tries(&waiter[i], MXSOCK_WAIT_SID + (u32)i, 2) < 0 || !waiter[i].server)) {
    int was = mx_di();
    waiter_none |= 1u << i; /* an older mxsock */
    mx_irq_restore(was);
    return -2;
  }
  volatile u32 *a = MX_UNCACHED(wait_buf[i]);
  a[0] = rd, a[1] = wr, a[2] = us, a[3] = 0;
  a[4] = (u32)-1;
  if (mx_sif_call(&waiter[i], 0, (void *)a, 16, (void *)(a + 4), 16) < 0)
    return -1;
  int n = (s32)a[4];
  *rr = n > 0 ? a[5] : 0;
  *rw = n > 0 ? a[6] : 0;
  return n;
}

/* Without a wait service: each socket asked, and a short sleep between rounds */
static int poll_once(u32 rd, u32 wr, u32 *rr, u32 *rw) {
  int n = 0;
  *rr = *rw = 0;
  for (int s = 0; s < MXSOCK_SOCKETS; s++) {
    int want = (rd >> s & 1 ? MX_NET_READ : 0) | (wr >> s & 1 ? MX_NET_WRITE : 0);
    if (!want)
      continue;
    int got = mx_net_poll(s, want);
    if (got > 0 && got & MX_NET_READ)
      *rr |= 1u << s, n++;
    if (got > 0 && got & MX_NET_WRITE)
      *rw |= 1u << s, n++;
  }
  return n;
}

int mx_net_wait(u32 rd, u32 wr, u32 timeout_us, u32 *ready_rd, u32 *ready_wr) {
  u32 rr = 0, rw = 0, t0 = mx_count(), left = timeout_us;
  int n;
  if (mx_net_init() < 0)
    return -1;
  for (;;) {
    u32 slice = left > MXSOCK_WAIT_MAX_US ? MXSOCK_WAIT_MAX_US : left;
    int i = waiter_take();
    n = -2;
    if (i >= 0) {
      n = wait_once(i, rd, wr, slice, &rr, &rw);
      waiter_give(i);
    }
    if (n == -2) {
      n = poll_once(rd, wr, &rr, &rw);
      if (!n && slice)
        mx_sleep_us(slice < 1000 ? slice : 1000);
    }
    if (n != 0)
      break;
    if (timeout_us != MX_NET_FOREVER) {
      u32 spent = (mx_count() - t0) / (MX_EE_HZ / 1000000u);
      if (spent >= timeout_us)
        break;
      left = timeout_us - spent;
      t0 += spent * (MX_EE_HZ / 1000000u), timeout_us = left;
    }
  }
  if (ready_rd)
    *ready_rd = n > 0 ? rr : 0;
  if (ready_wr)
    *ready_wr = n > 0 ? rw : 0;
  return n;
}
