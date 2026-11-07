#ifndef MX_NET_H
#define MX_NET_H

/* Sockets, on the IOP's TCP/IP stack (common/include/mxsock.h). No call but
   mx_net_wait waits: one that cannot go on now returns MX_NET_AGAIN, so a game can
   poll each frame. To wait for a socket, mx_net_wait: the thread sleeps meanwhile.
   Addresses are IPv4 in network order (mx_net_ip), ports in host order. What a
   program leaves open is closed when it ends. Threads may share the calls; not for
   interrupt handlers. */

#include <mx/types.h>

#define MX_NET_AGAIN (-11)
#define MX_NET_CLOSED (-104) /* by the other side, or an error on the connection */
#define MX_NET_READ 1
#define MX_NET_WRITE 2
#define MX_NET_TCP 1
#define MX_NET_UDP 2
#define MX_NET_MAX 4096u /* the most one send or recv moves */
#define MX_NET_FOREVER 0xFFFFFFFFu /* mx_net_wait's timeout: no end */

#define mx_net_ip(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)

/* 0, < 0 if the IOP has no socket service */
int mx_net_init(void);
int mx_net_socket(int type);
int mx_net_bind(int s, u32 port);
int mx_net_listen(int s, int backlog);
/* a new socket; ip and port (if not 0) say who */
int mx_net_accept(int s, u32 *ip, u32 *port);
/* AGAIN until the connection is up: call again until 0 or another error */
int mx_net_connect(int s, u32 ip, u32 port);
/* bytes taken: at most MX_NET_MAX, maybe fewer than size */
int mx_net_send(int s, const void *data, u32 size);
/* bytes received, 0 at the end of a connection */
int mx_net_recv(int s, void *data, u32 size);
int mx_net_sendto(int s, const void *data, u32 size, u32 ip, u32 port);
int mx_net_recvfrom(int s, void *data, u32 size, u32 *ip, u32 *port);
int mx_net_close(int s);
/* which of want (MX_NET_READ, MX_NET_WRITE) can go on now */
int mx_net_poll(int s, int want);
/* Sleep until one of the sockets can go on: bit s of rd for reading (data, a
   connection to accept, the end), of wr for writing (also: connect is done), or
   timeout_us has passed (0: only look). How many are ready, 0 at the timeout, < 0
   on an error; ready_rd and ready_wr (if not 0) get their bits. Sleeps on the IOP's
   wait service; with an older mxsock it asks each socket every millisecond. Up to
   four threads sleep at the IOP at once, more ask in turn. */
int mx_net_wait(u32 rd, u32 wr, u32 timeout_us, u32 *ready_rd, u32 *ready_wr);

#endif
