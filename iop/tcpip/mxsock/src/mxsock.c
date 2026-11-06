/* mxsock: sockets for the EE (common/include/mxsock.h). Before each call select
   says whether it can go on, so that nothing here waits. */
#include "irx_imports.h"
#include <mxsock.h>
#include <sifrpc.h>

IRX_ID("mxsock", 1, 0);

static struct {
  int fd; /* lwIP's + 1, 0: free */
  int connecting;
} sockets[MXSOCK_SOCKETS];

static u32 rpc_in[MXSOCK_ARGS + MXSOCK_MAX / 4] __attribute__((aligned(16)));
static s32 rpc_out[MXSOCK_ANSWER + MXSOCK_MAX / 4] __attribute__((aligned(16)));

static int ready(int fd, int want) {
  fd_set r, w;
  struct timeval now = {0, 0};
  FD_ZERO(&r);
  FD_ZERO(&w);
  if (want & MXSOCK_READ)
    FD_SET(fd, &r);
  if (want & MXSOCK_WRITE)
    FD_SET(fd, &w);
  if (lwip_select(fd + 1, want & MXSOCK_READ ? &r : 0, want & MXSOCK_WRITE ? &w : 0, 0, &now) <= 0)
    return 0;
  return (FD_ISSET(fd, &r) ? MXSOCK_READ : 0) | (FD_ISSET(fd, &w) ? MXSOCK_WRITE : 0);
}

static int add(int fd) {
  int on = 1;
  for (int s = 0; s < MXSOCK_SOCKETS; s++)
    if (!sockets[s].fd) {
      lwip_ioctl(fd, FIONBIO, &on);
      sockets[s].fd = fd + 1;
      sockets[s].connecting = 0;
      return s;
    }
  lwip_close(fd);
  return -24;
}

static void address(struct sockaddr_in *a, u32 ip, u32 port) {
  memset(a, 0, sizeof *a);
  a->sin_family = AF_INET;
  a->sin_port = htons(port);
  a->sin_addr.s_addr = ip;
}

static int do_connect(int s, int fd, u32 ip, u32 port) {
  if (!sockets[s].connecting) {
    struct sockaddr_in a;
    address(&a, ip, port);
    if (lwip_connect(fd, (struct sockaddr *)&a, sizeof a) == 0)
      return 0;
    sockets[s].connecting = 1;
    return MXSOCK_AGAIN;
  }
  if (!(ready(fd, MXSOCK_WRITE) & MXSOCK_WRITE))
    return MXSOCK_AGAIN;
  int err = 0, len = sizeof err;
  lwip_getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
  sockets[s].connecting = 0;
  return err ? MXSOCK_CLOSED : 0;
}

static void *rpc(int fno, void *data, int size) {
  u32 *a = data;
  u8 *in = (u8 *)(a + MXSOCK_ARGS), *out = (u8 *)(rpc_out + MXSOCK_ANSWER);
  s32 *r = rpc_out;
  (void)size;
  if (fno == MXSOCK_SOCKET) {
    int fd = lwip_socket(AF_INET, a[0] == MXSOCK_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    r[0] = fd < 0 ? -24 : add(fd);
    return r;
  }
  if (fno == MXSOCK_CLOSE_ALL) {
    for (int s = 0; s < MXSOCK_SOCKETS; s++)
      if (sockets[s].fd) {
        lwip_close(sockets[s].fd - 1);
        sockets[s].fd = 0;
      }
    r[0] = 0;
    return r;
  }
  int s = (int)a[0];
  if (s < 0 || s >= MXSOCK_SOCKETS || !sockets[s].fd) {
    r[0] = MXSOCK_BADF;
    return r;
  }
  int fd = sockets[s].fd - 1, n;
  u32 len = a[1] > MXSOCK_MAX ? MXSOCK_MAX : a[1];
  struct sockaddr_in addr;
  int alen = sizeof addr;
  switch (fno) {
  case MXSOCK_BIND: {
    int on = 1;
    lwip_setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    address(&addr, 0, a[1]);
    r[0] = lwip_bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0 ? MXSOCK_CLOSED : 0;
    break;
  }
  case MXSOCK_LISTEN:
    r[0] = lwip_listen(fd, (int)a[1]) < 0 ? MXSOCK_CLOSED : 0;
    break;
  case MXSOCK_ACCEPT:
    if (!(ready(fd, MXSOCK_READ) & MXSOCK_READ)) {
      r[0] = MXSOCK_AGAIN;
      break;
    }
    n = lwip_accept(fd, (struct sockaddr *)&addr, &alen);
    if (n < 0) {
      r[0] = MXSOCK_AGAIN;
      break;
    }
    r[0] = add(n);
    r[1] = (s32)addr.sin_addr.s_addr, r[2] = ntohs(addr.sin_port);
    break;
  case MXSOCK_CONNECT:
    r[0] = do_connect(s, fd, a[1], a[2]);
    break;
  case MXSOCK_SEND:
    if (!(ready(fd, MXSOCK_WRITE) & MXSOCK_WRITE)) {
      r[0] = MXSOCK_AGAIN;
      break;
    }
    n = lwip_send(fd, in, (int)len, MSG_DONTWAIT);
    r[0] = n < 0 ? MXSOCK_CLOSED : n;
    break;
  case MXSOCK_RECV:
    if (!(ready(fd, MXSOCK_READ) & MXSOCK_READ)) {
      r[0] = MXSOCK_AGAIN;
      break;
    }
    n = lwip_recv(fd, out, (int)len, MSG_DONTWAIT);
    r[0] = n < 0 ? MXSOCK_CLOSED : n;
    break;
  case MXSOCK_SENDTO:
    address(&addr, a[2], a[3]);
    n = lwip_sendto(fd, in, (int)len, 0, (struct sockaddr *)&addr, sizeof addr);
    r[0] = n < 0 ? MXSOCK_AGAIN : n;
    break;
  case MXSOCK_RECVFROM:
    if (!(ready(fd, MXSOCK_READ) & MXSOCK_READ)) {
      r[0] = MXSOCK_AGAIN;
      break;
    }
    n = lwip_recvfrom(fd, out, (int)len, MSG_DONTWAIT, (struct sockaddr *)&addr, &alen);
    r[0] = n < 0 ? MXSOCK_AGAIN : n;
    r[1] = (s32)addr.sin_addr.s_addr, r[2] = ntohs(addr.sin_port);
    break;
  case MXSOCK_CLOSE:
    r[0] = lwip_close(fd) < 0 ? MXSOCK_CLOSED : 0;
    sockets[s].fd = 0;
    break;
  case MXSOCK_POLL:
    r[0] = ready(fd, (int)a[1]);
    break;
  default:
    r[0] = -22;
  }
  return r;
}

static void thread(void *arg) {
  static SifRpcDataQueue_t queue;
  static SifRpcServerData_t server;
  (void)arg;
  sceSifSetRpcQueue(&queue, GetThreadId());
  sceSifRegisterRpc(&server, MXSOCK_SID, rpc, rpc_in, 0, 0, &queue);
  sceSifRpcLoop(&queue);
}

int _start(int argc, char **argv) {
  (void)argc, (void)argv;
  iop_thread_t t = {TH_C, 0, thread, 0x1000, 0x50};
  sceSifInitRpc(0);
  int id = CreateThread(&t);
  if (id <= 0)
    return MODULE_NO_RESIDENT_END;
  StartThread(id, 0);
  return MODULE_RESIDENT_END;
}
