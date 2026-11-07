/* What mxfast.c and console.c share */
#ifndef MXFAST_INT_H
#define MXFAST_INT_H

#include <types.h>

/* What a frame to the host starts with: its MAC, ours, type; at 26 our IP,
   at 30 the host's. Valid once have_peer is set. */
extern u8 fast_eth_ip[42];
extern volatile int fast_have_peer;

/* The Ethernet, IP and UDP headers of a frame to the host, udp bytes after them */
void fast_headers(u8 *f, u32 udp);
void fast_header(u8 *b, u32 op, u32 id, u32 a, u32 v, u32 c);

/* console.c, from rx in the driver thread */
void console_hello(u32 session);
void console_ack(u32 have);
int console_start(void);

#endif
