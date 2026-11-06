/* BSD sockets on <mx/net.h>: IPv4 on the LAN. Add -I$(MAXSDK)/ee/compat/net
   (MX_NET_CFLAGS in mk/maxsdk.mk). Implemented in libmxc (ee/libc/net.c).
   Sockets block unless made non-blocking (fcntl, ioctl FIONBIO, MSG_DONTWAIT);
   blocking gives other threads the time. */
#ifndef _SYS_SOCKET_H
#define _SYS_SOCKET_H

#include <stddef.h>
#include <stdint.h>
#include <sys/select.h>
#include <sys/types.h>

typedef uint32_t socklen_t;
typedef uint16_t sa_family_t;

struct sockaddr {
  sa_family_t sa_family;
  char sa_data[14];
};

#define AF_UNSPEC 0
#define AF_INET 2
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 0xffff
#define SO_REUSEADDR 0x0004
#define SO_KEEPALIVE 0x0008
#define SO_BROADCAST 0x0020
#define SO_LINGER 0x0080
#define SO_SNDBUF 0x1001
#define SO_RCVBUF 0x1002
#define SO_ERROR 0x1007
#define MSG_PEEK 0x02
#define MSG_DONTWAIT 0x40
#define MSG_NOSIGNAL 0
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
#define SOMAXCONN 8

struct linger {
  int l_onoff, l_linger;
};

#ifdef __cplusplus
extern "C" {
#endif
int socket(int domain, int type, int protocol);
int bind(int s, const struct sockaddr *addr, socklen_t len);
int listen(int s, int backlog);
int accept(int s, struct sockaddr *addr, socklen_t *len);
int connect(int s, const struct sockaddr *addr, socklen_t len);
ssize_t send(int s, const void *data, size_t size, int flags);
ssize_t recv(int s, void *data, size_t size, int flags);
ssize_t sendto(int s, const void *data, size_t size, int flags, const struct sockaddr *to,
               socklen_t len);
ssize_t recvfrom(int s, void *data, size_t size, int flags, struct sockaddr *from, socklen_t *len);
int shutdown(int s, int how);
int setsockopt(int s, int level, int name, const void *value, socklen_t len);
int getsockopt(int s, int level, int name, void *value, socklen_t *len);
int getsockname(int s, struct sockaddr *addr, socklen_t *len);
int getpeername(int s, struct sockaddr *addr, socklen_t *len);
int closesocket(int s);
#ifdef __cplusplus
}
#endif

#endif
