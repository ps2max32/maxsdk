/* The network's state and set-up, between mxnet (the IOP's TCP/IP stack),
   mxsock (its service for the EE) and the kernel (kernel version 30,
   <mx/net.h>'s mx_net_status).

   mxsock's MXSOCK_NET_WATCH gives an EE address for a struct mxnet_status:
   mxsock looks at the stack twice a second and writes it there by DMA when
   something changed (link, address, DHCP), with gen one more.
   MXSOCK_NET_CONFIG sets the stack up: DHCP, or a fixed address. */
#ifndef MXNET_H
#define MXNET_H

#define MXNET_STATUS_VERSION 1

/* link: mxeth's MXETH_LINK_* bits; these sum them up */
#define MXNET_LINK_100 0x0C /* 100 Mbit/s, half or full duplex */
#define MXNET_LINK_FDX 0x0A /* full duplex, 10 or 100 */

/* dhcp */
#define MXNET_DHCP_OFF 0
#define MXNET_DHCP_SEARCHING 1 /* asking for an address */
#define MXNET_DHCP_BOUND 2     /* has one */

/* 64 bytes; addresses in network order */
struct mxnet_status {
  u32 version; /* MXNET_STATUS_VERSION; 0: not written yet */
  u32 gen;     /* one more at each change */
  u32 link;    /* mxeth's MXETH_LINK_* bits; 0: no cable or no link */
  u32 ip, mask, gw, dns;
  u32 dhcp;  /* MXNET_DHCP_* */
  u32 lease; /* DHCP: seconds of the lease left when this was written; 0 none */
  u8 mac[6];
  u8 pad[2];
  u32 reserved[5];
};

/* MXSOCK_NET_CONFIG's words */
struct mxnet_config {
  u32 dhcp; /* 1: DHCP (ip, mask, gw: used until it has an address), 0: fixed */
  u32 ip, mask, gw, dns; /* dns 0: DHCP's, or none */
};

#ifdef _IOP
/* mxnet's (the ps2ip library): the state now (gen not set); the set-up, 0 */
void mxnet_status(struct mxnet_status *s);
int mxnet_config(const struct mxnet_config *c);
#endif

#endif
