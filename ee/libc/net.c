/* BSD sockets on <mx/net.h> (ee/compat/net). A socket's descriptor is
   SOCK_FD + its number, below FD_SETSIZE for select. mxsock's calls never wait:
   blocking is done here, by sleeping in mx_net_wait until the socket can go on. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <mx/net.h>
#include <mx/thread.h>
#include <mx/timer.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#define SOCK_FD 40
#define SOCKETS 32

static struct {
  int open, nonblock, stream;
  u32 ip, port, local_port;
} sock[SOCKETS];

static int number(int fd) {
  int s = fd - SOCK_FD;
  if (s < 0 || s >= SOCKETS || !sock[s].open) {
    errno = EBADF;
    return -1;
  }
  return s;
}

static int fail(int r) {
  errno = r == MX_NET_AGAIN ? EAGAIN : r == MX_NET_CLOSED ? ECONNRESET : EIO;
  return -1;
}

/* sleep until socket s can read (or accept), or write (or has connected) */
static void wait_for(int s, int want) {
  u32 bit = 1u << s;
  mx_net_wait(want & MX_NET_READ ? bit : 0, want & MX_NET_WRITE ? bit : 0, MX_NET_FOREVER, 0, 0);
}

static int nonblocking(int s, int flags) { return sock[s].nonblock || flags & MSG_DONTWAIT; }

int socket(int domain, int type, int protocol) {
  (void)protocol;
  if (domain != AF_INET || (type != SOCK_STREAM && type != SOCK_DGRAM)) {
    errno = EAFNOSUPPORT;
    return -1;
  }
  int s = mx_net_socket(type == SOCK_STREAM ? MX_NET_TCP : MX_NET_UDP);
  if (s < 0 || s >= SOCKETS) {
    errno = EMFILE;
    return -1;
  }
  memset(&sock[s], 0, sizeof sock[s]);
  sock[s].open = 1, sock[s].stream = type == SOCK_STREAM;
  return SOCK_FD + s;
}

int bind(int fd, const struct sockaddr *addr, socklen_t len) {
  const struct sockaddr_in *a = (const void *)addr;
  int s = number(fd);
  (void)len;
  if (s < 0)
    return -1;
  u32 port = ntohs(a->sin_port);
  int r = mx_net_bind(s, port);
  if (r < 0)
    return fail(r);
  sock[s].local_port = port;
  return 0;
}

int listen(int fd, int backlog) {
  int s = number(fd);
  if (s < 0)
    return -1;
  int r = mx_net_listen(s, backlog);
  return r < 0 ? fail(r) : 0;
}

static void fill(struct sockaddr *addr, socklen_t *len, u32 ip, u32 port) {
  struct sockaddr_in a;
  if (!addr || !len)
    return;
  memset(&a, 0, sizeof a);
  a.sin_family = AF_INET;
  a.sin_port = htons((u16)port);
  a.sin_addr.s_addr = ip;
  memcpy(addr, &a, *len < sizeof a ? *len : sizeof a);
  *len = sizeof a;
}

int accept(int fd, struct sockaddr *addr, socklen_t *len) {
  int s = number(fd), r;
  u32 ip = 0, port = 0;
  if (s < 0)
    return -1;
  while ((r = mx_net_accept(s, &ip, &port)) == MX_NET_AGAIN) {
    if (sock[s].nonblock)
      return fail(r);
    wait_for(s, MX_NET_READ);
  }
  if (r < 0 || r >= SOCKETS)
    return fail(r);
  memset(&sock[r], 0, sizeof sock[r]);
  sock[r].open = 1, sock[r].stream = 1, sock[r].ip = ip, sock[r].port = port;
  fill(addr, len, ip, port);
  return SOCK_FD + r;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
  const struct sockaddr_in *a = (const void *)addr;
  int s = number(fd), r;
  (void)len;
  if (s < 0)
    return -1;
  u32 ip = a->sin_addr.s_addr, port = ntohs(a->sin_port);
  sock[s].ip = ip, sock[s].port = port;
  if (!sock[s].stream)
    return 0; /* UDP: where send goes */
  while ((r = mx_net_connect(s, ip, port)) == MX_NET_AGAIN) {
    if (sock[s].nonblock) {
      errno = EINPROGRESS;
      return -1;
    }
    wait_for(s, MX_NET_WRITE);
  }
  return r < 0 ? fail(r) : 0;
}

ssize_t send(int fd, const void *data, size_t size, int flags) {
  int s = number(fd);
  if (s < 0)
    return -1;
  if (!sock[s].stream)
    return mx_net_sendto(s, data, size, sock[s].ip, sock[s].port);
  const char *p = data;
  size_t done = 0;
  while (done < size) {
    int r = mx_net_send(s, p + done, size - done);
    if (r == MX_NET_AGAIN) {
      if (nonblocking(s, flags))
        return done ? (ssize_t)done : fail(r);
      wait_for(s, MX_NET_WRITE);
      continue;
    }
    if (r < 0)
      return done ? (ssize_t)done : fail(r);
    done += (size_t)r;
  }
  return (ssize_t)done;
}

ssize_t recv(int fd, void *data, size_t size, int flags) {
  return recvfrom(fd, data, size, flags, 0, 0);
}

ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *to,
               socklen_t len) {
  const struct sockaddr_in *a = (const void *)to;
  int s = number(fd);
  (void)flags, (void)len;
  if (s < 0)
    return -1;
  if (!to)
    return send(fd, data, size, flags);
  int r;
  while ((r = mx_net_sendto(s, data, size, a->sin_addr.s_addr, ntohs(a->sin_port))) == MX_NET_AGAIN) {
    if (nonblocking(s, flags))
      return fail(r);
    wait_for(s, MX_NET_WRITE);
  }
  return r < 0 ? fail(r) : r;
}

ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *from, socklen_t *len) {
  int s = number(fd), r;
  u32 ip = 0, port = 0;
  if (s < 0)
    return -1;
  if (flags & MSG_PEEK) {
    errno = EOPNOTSUPP;
    return -1;
  }
  for (;;) {
    r = sock[s].stream ? mx_net_recv(s, data, size) : mx_net_recvfrom(s, data, size, &ip, &port);
    if (r != MX_NET_AGAIN)
      break;
    if (nonblocking(s, flags))
      return fail(r);
    wait_for(s, MX_NET_READ);
  }
  if (r < 0)
    return fail(r);
  fill(from, len, sock[s].stream ? sock[s].ip : ip, sock[s].stream ? sock[s].port : port);
  return r;
}

int shutdown(int fd, int how) {
  (void)how; /* lwIP's close does it */
  return number(fd) < 0 ? -1 : 0;
}

int setsockopt(int fd, int level, int name, const void *value, socklen_t len) {
  (void)level, (void)name, (void)value, (void)len;
  return number(fd) < 0 ? -1 : 0; /* none of them changes anything yet */
}

int getsockopt(int fd, int level, int name, void *value, socklen_t *len) {
  (void)level, (void)name;
  if (number(fd) < 0)
    return -1;
  if (value && len && *len >= sizeof(int))
    *(int *)value = 0;
  return 0;
}

int getsockname(int fd, struct sockaddr *addr, socklen_t *len) {
  int s = number(fd);
  if (s < 0)
    return -1;
  fill(addr, len, 0, sock[s].local_port);
  return 0;
}

int getpeername(int fd, struct sockaddr *addr, socklen_t *len) {
  int s = number(fd);
  if (s < 0)
    return -1;
  fill(addr, len, sock[s].ip, sock[s].port);
  return 0;
}

int closesocket(int fd) {
  int s = number(fd);
  if (s < 0)
    return -1;
  sock[s].open = 0;
  int r = mx_net_close(s);
  return r < 0 ? fail(r) : 0;
}

/* Weak: a port may have its own, and call closesocket for sockets */
__attribute__((weak)) int close(int fd) { return closesocket(fd); }

__attribute__((weak)) int fcntl(int fd, int cmd, ...) {
  int s = number(fd);
  if (s < 0)
    return -1;
  if (cmd == F_GETFL)
    return sock[s].nonblock ? O_NONBLOCK : 0;
  if (cmd == F_SETFL) {
    va_list ap;
    va_start(ap, cmd);
    sock[s].nonblock = !!(va_arg(ap, int) & O_NONBLOCK);
    va_end(ap);
  }
  return 0;
}

__attribute__((weak)) int ioctl(int fd, unsigned long request, ...) {
  int s = number(fd);
  va_list ap;
  if (s < 0)
    return -1;
  va_start(ap, request);
  int *arg = va_arg(ap, int *);
  va_end(ap);
  if (request == FIONBIO) {
    sock[s].nonblock = !!*arg;
    return 0;
  }
  errno = EINVAL;
  return -1;
}

int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *timeout) {
  u32 rd = 0, wr = 0, rr = 0, rw = 0;
  u32 us = timeout ? (u32)timeout->tv_sec * 1000000u + (u32)timeout->tv_usec : MX_NET_FOREVER;
  for (int fd = SOCK_FD; fd < n && fd < SOCK_FD + SOCKETS; fd++) {
    int s = fd - SOCK_FD;
    if (!sock[s].open)
      continue;
    if (r && FD_ISSET(fd, r))
      rd |= 1u << s;
    if (w && FD_ISSET(fd, w))
      wr |= 1u << s;
  }
  if (e)
    FD_ZERO(e);
  if (r)
    FD_ZERO(r);
  if (w)
    FD_ZERO(w);
  if (!rd && !wr) {
    if (us != MX_NET_FOREVER)
      mx_sleep_us(us);
    return 0;
  }
  int got = mx_net_wait(rd, wr, us, &rr, &rw);
  if (got < 0) {
    errno = EIO;
    return -1;
  }
  int ready = 0;
  for (int s = 0; s < SOCKETS; s++) {
    if (r && rr >> s & 1)
      FD_SET(SOCK_FD + s, r), ready++;
    if (w && rw >> s & 1)
      FD_SET(SOCK_FD + s, w), ready++;
  }
  return ready;
}

struct hostent *gethostbyname(const char *name) {
  static struct hostent h;
  static u32 ip;
  static char *list[2] = {(char *)&ip, 0};
  u32 part = 0, parts = 0, value = 0;
  for (const char *p = name;; p++) {
    if (*p == '.' || !*p) {
      if (part > 255)
        return 0;
      value |= part << (8 * parts++), part = 0;
      if (!*p)
        break;
    } else if (*p >= '0' && *p <= '9')
      part = part * 10 + (u32)(*p - '0');
    else
      return 0;
  }
  if (parts != 4)
    return 0;
  ip = value;
  h.h_name = (char *)name, h.h_addrtype = AF_INET, h.h_length = 4, h.h_addr_list = list;
  return &h;
}
