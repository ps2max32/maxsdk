#ifndef _NETINET_IN_H
#define _NETINET_IN_H

#include <arpa/inet.h>
#include <stdint.h>
#include <sys/socket.h>

typedef uint32_t in_addr_t;
typedef uint16_t in_port_t;

#ifndef _STRUCT_IN_ADDR
#define _STRUCT_IN_ADDR
struct in_addr {
  in_addr_t s_addr;
};
#endif

struct sockaddr_in {
  sa_family_t sin_family;
  in_port_t sin_port;
  struct in_addr sin_addr;
  unsigned char sin_zero[8];
};

#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY ((in_addr_t)0)
#define INADDR_NONE ((in_addr_t)0xffffffff)
#define INADDR_BROADCAST ((in_addr_t)0xffffffff)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#define INET_ADDRSTRLEN 16

#endif
