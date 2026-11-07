# The TCP/IP stack: lwIP (libs/lwip), as ps2sdk's ps2ip-nm.irx builds it, with the
# same exports (library "ps2ip").

# ps2ip-nm.irx, on ps2sdk's netman, has the fast path in it (mxfast.c,
# common/include/mxfast.h). MXETH=1: mxnet.irx, on maxsdk's mxeth
# (docs/iop-drivers.md), without it: there it is a module of its own,
# iop/tcpip/mxfast. Both are built.
ifdef MXETH
MODULE = mxnet
CFLAGS_MXETH = -DMXNET_MXETH -I../../dev9/mxeth/include
SRCS   = ps2ip.c sys_arch.c
else
MODULE = ps2ip-nm
SRCS   = ps2ip.c mxfast.c sys_arch.c
all: mxnet-on-mxeth
mxnet-on-mxeth:
	$(MAKE) MXETH=1
.PHONY: mxnet-on-mxeth
endif

LWIP ?= $(abspath ../../../../lwip)
CFLAGS_MODULE = -DLWIP_NOASSERT -DLWIP_COMPAT_MUTEX -DLWIP_COMPAT_MUTEX_ALLOWED -DPS2IP_DHCP \
                -DPS2IP_DNS -I$(LWIP)/src/include -I$(LWIP)/src/include/ipv4 $(CFLAGS_MXETH)

LWIP_SRCS = api/api_lib.c api/api_msg.c api/err.c api/netbuf.c api/netdb.c api/sockets.c api/tcpip.c \
            core/def.c core/dns.c core/inet_chksum.c core/init.c core/ip.c core/mem.c core/memp.c \
            core/netif.c core/pbuf.c core/raw.c core/stats.c core/sys.c core/tcp.c core/tcp_in.c \
            core/tcp_out.c core/timeouts.c core/udp.c \
            core/ipv4/dhcp.c core/ipv4/etharp.c core/ipv4/icmp.c core/ipv4/ip4.c \
            core/ipv4/ip4_addr.c core/ipv4/ip4_frag.c netif/ethernet.c
OBJS_MODULE = $(patsubst %.c,$(OBJ)/lwip/%.o,$(LWIP_SRCS))

include ../../Rules.mk

$(OBJ)/lwip/%.o: $(LWIP)/src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
