/* Sockets: the EE's side of common/include/mxsock.h (<mx/net.h>). */
#include <mx/cpu.h>
#include <mx/net.h>
#include <mx/sif.h>
#include <mx/string.h>
#include "../../../common/include/mxsock.h"

static struct mx_sif_client srv;
static u32 in[MXSOCK_ARGS + MXSOCK_MAX / 4] MX_DMA_LINES;
static s32 out[MXSOCK_ANSWER + MXSOCK_MAX / 4] MX_DMA_LINES;

int mx_net_init(void) {
  if (mx_sif_bound(&srv))
    return 0;
  if (mx_sif_init() < 0 || mx_sif_bind_tries(&srv, MXSOCK_SID, 100) < 0 || !srv.server)
    return -1;
  return 0;
}

/* The call: bytes of data in in, wanted back in out */
static int call(int fno, u32 a0, u32 a1, u32 a2, u32 a3, u32 send, u32 back) {
  if (mx_net_init() < 0)
    return -1;
  in[0] = a0, in[1] = a1, in[2] = a2, in[3] = a3;
  mx_dcache_writeback(in, sizeof in);
  volatile s32 *r = MX_UNCACHED(out);
  r[0] = -1;
  u32 ssize = (MXSOCK_ARGS * 4 + send + 15) & ~15u;
  u32 rsize = (MXSOCK_ANSWER * 4 + back + 15) & ~15u;
  if (mx_sif_call(&srv, fno, in, ssize, (void *)r, rsize) < 0)
    return -1;
  return r[0];
}

int mx_net_socket(int type) { return call(MXSOCK_SOCKET, (u32)type, 0, 0, 0, 0, 0); }
int mx_net_bind(int s, u32 port) { return call(MXSOCK_BIND, (u32)s, port, 0, 0, 0, 0); }
int mx_net_listen(int s, int backlog) { return call(MXSOCK_LISTEN, (u32)s, (u32)backlog, 0, 0, 0, 0); }
int mx_net_connect(int s, u32 ip, u32 port) { return call(MXSOCK_CONNECT, (u32)s, ip, port, 0, 0, 0); }
int mx_net_close(int s) { return call(MXSOCK_CLOSE, (u32)s, 0, 0, 0, 0, 0); }
int mx_net_poll(int s, int want) { return call(MXSOCK_POLL, (u32)s, (u32)want, 0, 0, 0, 0); }

void mx_net_close_all(void) {
  /* a service that is not there has nothing open */
  if (mx_sif_bound(&srv) || (mx_sif_init() == 0 && mx_sif_bind_tries(&srv, MXSOCK_SID, 1) == 0 && srv.server))
    call(MXSOCK_CLOSE_ALL, 0, 0, 0, 0, 0, 0);
}

static void who(u32 *ip, u32 *port) {
  const volatile s32 *r = MX_UNCACHED(out);
  if (ip)
    *ip = (u32)r[1];
  if (port)
    *port = (u32)r[2];
}

int mx_net_accept(int s, u32 *ip, u32 *port) {
  int n = call(MXSOCK_ACCEPT, (u32)s, 0, 0, 0, 0, 0);
  if (n >= 0)
    who(ip, port);
  return n;
}

static u32 limit(u32 size) { return size > MXSOCK_MAX ? MXSOCK_MAX : size; }

int mx_net_send(int s, const void *data, u32 size) {
  size = limit(size);
  memcpy(in + MXSOCK_ARGS, data, size);
  return call(MXSOCK_SEND, (u32)s, size, 0, 0, size, 0);
}

int mx_net_sendto(int s, const void *data, u32 size, u32 ip, u32 port) {
  size = limit(size);
  memcpy(in + MXSOCK_ARGS, data, size);
  return call(MXSOCK_SENDTO, (u32)s, size, ip, port, size, 0);
}

static int take(int n, void *data) {
  if (n > 0)
    memcpy(data, (const void *)MX_UNCACHED(out + MXSOCK_ANSWER), (u32)n);
  return n;
}

int mx_net_recv(int s, void *data, u32 size) {
  size = limit(size);
  return take(call(MXSOCK_RECV, (u32)s, size, 0, 0, 0, size), data);
}

int mx_net_recvfrom(int s, void *data, u32 size, u32 *ip, u32 *port) {
  size = limit(size);
  int n = take(call(MXSOCK_RECVFROM, (u32)s, size, 0, 0, 0, size), data);
  if (n >= 0)
    who(ip, port);
  return n;
}
