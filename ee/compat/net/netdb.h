/* Names: gethostbyname asks the DNS (mx_net_resolve, <mx/net.h>); IPv4 only. */
#ifndef _NETDB_H
#define _NETDB_H

#include <netinet/in.h>

struct hostent {
  char *h_name;
  char **h_aliases;
  int h_addrtype;
  int h_length;
  char **h_addr_list;
};
#define h_addr h_addr_list[0]

#define HOST_NOT_FOUND 1
#ifdef __cplusplus
extern "C" int h_errno;
#else
extern int h_errno;
#endif

#ifdef __cplusplus
extern "C"
#endif
struct hostent *gethostbyname(const char *name);

#endif
