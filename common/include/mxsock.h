/* mxsock: sockets for the EE, on lwIP on the IOP.

   No call waits: one that cannot go on now answers MXSOCK_AGAIN, so the service
   cannot hang and a game can poll each frame. Waiting is the EE's. Addresses are
   IPv4 in network order, ports in host order.

   A call's arguments are MXSOCK_ARGS words, then its data; the answer is
   MXSOCK_ANSWER words, then its data. */
#ifndef MXSOCK_H
#define MXSOCK_H

#define MXSOCK_SID 0x4D58534B /* "MXSK" */
#define MXSOCK_ARGS 8
#define MXSOCK_ANSWER 4
#define MXSOCK_MAX 4096u /* bytes of data in one call */
#define MXSOCK_SOCKETS 32

#define MXSOCK_AGAIN (-11)
#define MXSOCK_CLOSED (-104) /* by the other side, or an error on the connection */
#define MXSOCK_BADF (-9)

#define MXSOCK_TCP 1
#define MXSOCK_UDP 2

/* for MXSOCK_POLL */
#define MXSOCK_READ 1
#define MXSOCK_WRITE 2

enum {
  MXSOCK_SOCKET,  /* type; answer: socket */
  MXSOCK_BIND,    /* socket, port */
  MXSOCK_LISTEN,  /* socket, backlog */
  MXSOCK_ACCEPT,  /* socket; answer: socket, ip, port */
  MXSOCK_CONNECT, /* socket, ip, port: AGAIN until it is up, then 0 */
  MXSOCK_SEND,    /* socket, size, data; answer: bytes taken */
  MXSOCK_RECV,    /* socket, size; answer: bytes, data; 0 at the end */
  MXSOCK_SENDTO,  /* socket, size, ip, port, data; answer: bytes */
  MXSOCK_RECVFROM, /* socket, size; answer: bytes, ip, port, data */
  MXSOCK_CLOSE,   /* socket */
  MXSOCK_POLL,    /* socket, MXSOCK_READ | MXSOCK_WRITE; answer: those ready */
  MXSOCK_CLOSE_ALL /* what a program left open */
};

#endif
